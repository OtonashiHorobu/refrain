// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// RIDL extension semantics: parser/AST shape, semantic validation, stable id
// derivation, generated C ABI/C++ surface and the "no extension declared, no
// ABI change" guarantee.

#include <schema/schema.hpp>

#include <schema/schema_test_util.hpp>

#include <boost/ut.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace boost::ut;
using refrain::schema::dialect;
using refrain::schema::generated_file;
using refrain::schema::generation;
using refrain::schema::model;
using refrain::schema::module_model;
using refrain::test::compile;
using refrain::test::schema_source;

[[nodiscard]] auto ridl_source(std::string path, std::string text)
    -> schema_source {
    return schema_source{std::move(path), dialect::ridl, std::move(text)};
}

[[nodiscard]] auto ridl_file(std::string path, std::string text)
    -> refrain::schema::source_file {
    return refrain::schema::source_file{std::move(path), dialect::ridl,
                                        std::move(text)};
}

[[nodiscard]] auto find_module(const model &program, std::string_view name)
    -> const module_model * {
    const auto found{std::ranges::find_if(
        program.modules,
        [name](const module_model &module) { return module.name == name; })};
    return found == program.modules.end() ? nullptr : &*found;
}

[[nodiscard]] auto find_file(const generation &result, std::string_view path)
    -> const std::string * {
    const auto found{
        std::ranges::find_if(result.files, [path](const generated_file &file) {
            return file.path == path;
        })};
    return found == result.files.end() ? nullptr : &found->content;
}

void expect_rejected(const refrain::test::compile_result &result,
                     std::string_view marker) {
    expect(!result.program.has_value()) << "expected rejection: " << marker;
    expect(result.diagnostics.find(marker) != std::string::npos)
        << "missing diagnostic '" << marker << "' in: " << result.diagnostics;
}

/// Drop the schema-hash banner line so two generations over different source
/// sets can be compared for ABI/output stability.
[[nodiscard]] auto without_schema_hash(std::string text) -> std::string {
    constexpr std::string_view prefix{"// Schema hash: "};
    if (const auto position{text.find(prefix)}; position != std::string::npos) {
        const auto end{text.find('\n', position)};
        text.erase(position, end == std::string::npos ? std::string::npos
                                                      : end - position + 1);
    }
    return text;
}

constexpr std::string_view valid_extension_schema{
    R"(module refrain::services::extprobe;
version 1.0.0;

enum extprobe_error : u32 { failed = 1 }
struct payload { u32 value; }

extension chunk_codec : 1.2.3 {
    extends = "refrain:extprobe";
    capability = "chunk-codec";

    #[thread(worker)] #[can_fail]
    fn encode(in payload value, out payload encoded)
        -> result<void, extprobe_error>;
}

service extprobe : 1.1.0 {
    side = server;
    thread = worker;
    provides = [ "refrain:extprobe" ];
    extensions = [ chunk_codec ];

    fn ping() -> u32;
}
)"};

const suite schema_extension_tests = [] {
    "schema/extension parses metadata and resolves into the model"_test = [] {
        const auto result{compile({ridl_source(
            "extprobe.ridl", std::string{valid_extension_schema})})};
        expect(result.program.has_value())
            << "diagnostics: " << result.diagnostics;
        if (!result.program) {
            return;
        }
        const auto *module{
            find_module(*result.program, "refrain::services::extprobe")};
        expect(module != nullptr);
        if (module == nullptr) {
            return;
        }
        expect(module->declarations.size() == 4U);
        const auto &extension{module->declarations[2]};
        expect(extension.kind == refrain::schema::declaration_kind::extension);
        expect(extension.name == "chunk_codec");
        expect(extension.extension.version == "1.2.3");
        expect(extension.extension.extends_service == "refrain:extprobe");
        expect(extension.extension.capability == "chunk-codec");
        expect(extension.extension_id == "refrain:extprobe/chunk_codec");
        expect(extension.extension_version_major == 1U);
        expect(extension.extension_version_minor == 2U);
        expect(extension.extension_version_patch == 3U);
        expect(extension.methods.size() == 1U);
        expect(extension.methods.front().name == "encode");
        expect(extension.methods.front().can_fail);

        const auto &service{module->declarations[3]};
        expect(service.kind == refrain::schema::declaration_kind::service);
        expect(service.extension_points.size() == 1U);
        expect(service.extension_points.front().module_index == 0U);
        expect(service.extension_points.front().decl_index == 2U);
        expect(service.service.extensions.size() == 1U);
        expect(service.service.extensions.front() == "chunk_codec");
    };

    "schema/extension rejects malformed declarations"_test = [] {
        const auto unknown_parent{
            compile({ridl_source("orphan.ridl",
                                 R"(module refrain::services::orphan;
version 1.0.0;
extension lost : 1.0.0 {
    extends = "refrain:missing";
    capability = "lost";
    fn ping() -> u32;
}
)")})};
        expect_rejected(unknown_parent, "extends unknown service");

        const auto unknown_extension{
            compile({ridl_source("host.ridl",
                                 R"(module refrain::services::host;
version 1.0.0;
service host : 1.0.0 {
    side = server;
    thread = any;
    provides = [ "refrain:host" ];
    extensions = [ nope ];
    fn ping() -> u32;
}
)")})};
        expect_rejected(unknown_extension, "declares unknown extension");

        const auto duplicate_name{
            compile({ridl_source("dupname.ridl",
                                 R"(module refrain::services::dupname;
version 1.0.0;
extension codec : 1.0.0 {
    extends = "refrain:dupname";
    capability = "codec";
    fn ping() -> u32;
}
extension codec : 2.0.0 {
    extends = "refrain:dupname";
    capability = "codec";
    fn ping() -> u32;
}
service dupname : 1.0.0 {
    side = server;
    thread = any;
    provides = [ "refrain:dupname" ];
    extensions = [ codec ];
    fn ping() -> u32;
}
)")})};
        expect_rejected(duplicate_name, "duplicate declaration 'codec'");

        const auto bad_version{
            compile({ridl_source("badver.ridl",
                                 R"(module refrain::services::badver;
version 1.0.0;
extension codec : 1.0 {
    extends = "refrain:badver";
    capability = "codec";
    fn ping() -> u32;
}
service badver : 1.0.0 {
    side = server;
    thread = any;
    provides = [ "refrain:badver" ];
    extensions = [ codec ];
    fn ping() -> u32;
}
)")})};
        expect_rejected(bad_version, "invalid extension version");

        const auto bad_capability{
            compile({ridl_source("badcap.ridl",
                                 R"(module refrain::services::badcap;
version 1.0.0;
extension codec : 1.0.0 {
    extends = "refrain:badcap";
    capability = "Chunk Codec";
    fn ping() -> u32;
}
service badcap : 1.0.0 {
    side = server;
    thread = any;
    provides = [ "refrain:badcap" ];
    extensions = [ codec ];
    fn ping() -> u32;
}
)")})};
        expect_rejected(bad_capability, "invalid capability");

        const auto missing_capability{
            compile({ridl_source("nocap.ridl",
                                 R"(module refrain::services::nocap;
version 1.0.0;
extension codec : 1.0.0 {
    extends = "refrain:nocap";
    fn ping() -> u32;
}
service nocap : 1.0.0 {
    side = server;
    thread = any;
    provides = [ "refrain:nocap" ];
    extensions = [ codec ];
    fn ping() -> u32;
}
)")})};
        expect_rejected(missing_capability, "must declare capability");

        const auto oversized_version{
            compile({ridl_source("bigver.ridl",
                                 R"(module refrain::services::bigver;
version 1.0.0;
extension codec : 70000.0.0 {
    extends = "refrain:bigver";
    capability = "codec";
    fn ping() -> u32;
}
service bigver : 1.0.0 {
    side = server;
    thread = any;
    provides = [ "refrain:bigver" ];
    extensions = [ codec ];
    fn ping() -> u32;
}
)")})};
        expect_rejected(oversized_version, "cannot be encoded");
    };

    "schema/extension validates methods and service links"_test = [] {
        const auto bad_method_type{
            compile({ridl_source("badmethod.ridl",
                                 R"(module refrain::services::badmethod;
version 1.0.0;
struct payload { u32 value; }
extension codec : 1.0.0 {
    extends = "refrain:badmethod";
    capability = "codec";
    fn bad() -> array<payload>;
}
service badmethod : 1.0.0 {
    side = server;
    thread = any;
    provides = [ "refrain:badmethod" ];
    extensions = [ codec ];
    fn ping() -> u32;
}
)")})};
        expect_rejected(bad_method_type, "not representable in the C ABI");

        const auto scheduled{
            compile({ridl_source("sched.ridl",
                                 R"(module refrain::services::sched;
version 1.0.0;
extension codec : 1.0.0 {
    extends = "refrain:sched";
    capability = "codec";
    #[schedule(interval=1)] fn tick() -> void;
}
service sched : 1.0.0 {
    side = server;
    thread = any;
    provides = [ "refrain:sched" ];
    extensions = [ codec ];
    fn ping() -> u32;
}
)")})};
        expect_rejected(scheduled, "not on extension methods");

        const auto duplicate_entry{
            compile({ridl_source("dupentry.ridl",
                                 R"(module refrain::services::dupentry;
version 1.0.0;
extension codec : 1.0.0 {
    extends = "refrain:dupentry";
    capability = "codec";
    fn ping() -> u32;
}
service dupentry : 1.0.0 {
    side = server;
    thread = any;
    provides = [ "refrain:dupentry" ];
    extensions = [ codec, codec ];
    fn ping() -> u32;
}
)")})};
        expect_rejected(duplicate_entry, "more than once");

        const auto not_listed{
            compile({ridl_source("unlisted.ridl",
                                 R"(module refrain::services::unlisted;
version 1.0.0;
extension codec : 1.0.0 {
    extends = "refrain:unlisted";
    capability = "codec";
    fn ping() -> u32;
}
service unlisted : 1.0.0 {
    side = server;
    thread = any;
    provides = [ "refrain:unlisted" ];
    fn ping() -> u32;
}
)")})};
        expect_rejected(not_listed, "not listed by its parent service");

        const auto wrong_service{
            compile({ridl_source("child.ridl",
                                 R"(module refrain::services::child;
version 1.0.0;
extension codec : 1.0.0 {
    extends = "refrain:other";
    capability = "codec";
    fn ping() -> u32;
}
service other : 1.0.0 {
    side = server;
    thread = any;
    provides = [ "refrain:other" ];
    extensions = [ codec ];
    fn ping() -> u32;
}
)"),
                     ridl_source("parent.ridl",
                                 R"(module refrain::services::parent;
version 1.0.0;
import refrain::services::child;
service parent : 1.0.0 {
    side = server;
    thread = any;
    provides = [ "refrain:parent" ];
    extensions = [ codec ];
    fn ping() -> u32;
}
)")})};
        expect_rejected(wrong_service, "not this service");

        const auto duplicate_id{
            compile({ridl_source("host2.ridl",
                                 R"(module refrain::services::host2;
version 1.0.0;
service host2 : 1.0.0 {
    side = server;
    thread = any;
    provides = [ "refrain:host2" ];
    fn ping() -> u32;
}
)"),
                     ridl_source("a2.ridl",
                                 R"(module refrain::services::a2;
version 1.0.0;
extension same : 1.0.0 {
    extends = "refrain:host2";
    capability = "a";
    fn ping() -> u32;
}
)"),
                     ridl_source("b2.ridl",
                                 R"(module refrain::services::b2;
version 1.0.0;
extension same : 1.0.0 {
    extends = "refrain:host2";
    capability = "b";
    fn ping() -> u32;
}
)")})};
        expect_rejected(duplicate_id, "duplicate extension id");

        std::vector<schema_source> rsc_sources;
        rsc_sources.push_back(schema_source{"ext.rsc", dialect::rsc,
                                            R"(module refrain::rscext;
version 1;
extension codec : 1.0.0 {
    extends = "refrain:rscext";
    capability = "codec";
}
)"});
        const auto rsc_extension{compile(rsc_sources)};
        expect_rejected(rsc_extension, "only valid in RIDL");
    };

    // Two extensions on one service: both vtables, the extension-id table and
    // a single query slot are generated, in declaration/list order.
    "schema/generator emits C and C++ extension surfaces"_test = [] {
        std::vector<refrain::schema::source_file> sources;
        sources.push_back(ridl_file("multiext.ridl",
                                    R"(module refrain::services::multiext;
version 2.0.0;
struct payload { u32 value; }
extension alpha : 1.2.3 {
    extends = "refrain:multiext";
    capability = "alpha";
    fn one(u32 x) -> u32;
}
extension beta : 2.0.1 {
    extends = "refrain:multiext";
    capability = "beta";
    fn two() -> void;
}
service multiext : 1.4.0 {
    side = both;
    thread = any;
    provides = [ "refrain:multiext" ];
    extensions = [ alpha, beta ];
    fn ping() -> u32;
}
)"));
        const auto generated{
            refrain::schema::generate_from_sources(std::move(sources))};
        expect(generated.has_value())
            << "diagnostics: " << (generated ? "" : generated.error().message);
        if (!generated) {
            return;
        }
        const auto *abi{find_file(
            *generated, "c/refrain/contracts/services/multiext_abi.h")};
        expect(abi != nullptr);
        if (abi != nullptr) {
            expect(abi->find("REFRAIN_EXTENSION_ABI_DEFINED") !=
                   std::string::npos);
            expect(abi->find("typedef struct refrain_extension_ref") !=
                   std::string::npos);
            expect(abi->find("REFRAIN_SERVICES_MULTIEXT_ALPHA_ID "
                             "\"refrain:multiext/alpha\"") !=
                   std::string::npos);
            expect(abi->find("REFRAIN_SERVICES_MULTIEXT_BETA_ID "
                             "\"refrain:multiext/beta\"") != std::string::npos);
            expect(abi->find("typedef struct "
                             "refrain_services_multiext_alpha_vtable") !=
                   std::string::npos);
            expect(abi->find("typedef struct "
                             "refrain_services_multiext_beta_vtable") !=
                   std::string::npos);
            expect(abi->find("REFRAIN_SERVICES_MULTIEXT_ALPHA_ABI_MINOR 2u") !=
                   std::string::npos);
            expect(abi->find("REFRAIN_SERVICES_MULTIEXT_BETA_ABI_PATCH 1u") !=
                   std::string::npos);
            expect(abi->find("REFRAIN_SERVICES_MULTIEXT_EXTENSION_IDS "
                             "\"refrain:multiext/alpha,"
                             "refrain:multiext/beta\"") != std::string::npos);
            expect(
                abi->find("(*query_extension)(void *self, refrain_str id,") !=
                std::string::npos);
            // One query slot only, after the contract method slots.
            const auto first{abi->find("(*query_extension)")};
            const auto second{
                abi->find("(*query_extension)",
                          first == std::string::npos ? 0 : first + 1)};
            expect(first != std::string::npos);
            expect(second == std::string::npos);
            const auto ping{abi->find("(*ping)")};
            expect(ping != std::string::npos);
            if (ping != std::string::npos && first != std::string::npos) {
                expect(ping < first);
            }
        }

        const auto *cpp{find_file(
            *generated, "cpp/refrain/contracts/services/multiext.hpp")};
        expect(cpp != nullptr);
        if (cpp != nullptr) {
            expect(cpp->find("class alpha final") != std::string::npos);
            expect(cpp->find("class beta final") != std::string::npos);
            expect(
                cpp->find("static constexpr std::string_view extension_id =") !=
                std::string::npos);
            expect(cpp->find("\"refrain:multiext/alpha\"") !=
                   std::string::npos);
            expect(cpp->find("\"refrain:multiext/beta\"") != std::string::npos);
            expect(cpp->find("inline constexpr std::string_view "
                             "extension_ids[] = {") != std::string::npos);
            expect(cpp->find("class extension_registry final") !=
                   std::string::npos);
            expect(cpp->find("template <typename Extension>") !=
                   std::string::npos);
            expect(cpp->find("auto extension() const") != std::string::npos);
            expect(cpp->find("offsetof(service_vtable, query_extension)") !=
                   std::string::npos);
            expect(cpp->find("static constexpr std::uint32_t abi_version =") !=
                   std::string::npos);
        }

        const auto *reflection{
            find_file(*generated, "cpp/refrain/reflection.hpp")};
        expect(reflection != nullptr);
        if (reflection != nullptr) {
            expect(reflection->find(
                       "template <> struct custom_extension_traits<::refrain::"
                       "services::multiext::alpha>") != std::string::npos);
            expect(reflection->find(
                       "template <> struct custom_extension_traits<::refrain::"
                       "services::multiext::beta>") != std::string::npos);
            expect(reflection->find("static constexpr std::string_view id =") !=
                   std::string::npos);
            expect(reflection->find("\"refrain:multiext/alpha\"") !=
                   std::string::npos);
        }

        // Deterministic output.
        constexpr std::string_view multiext_schema{
            R"(module refrain::services::multiext;
version 2.0.0;
struct payload { u32 value; }
extension alpha : 1.2.3 {
    extends = "refrain:multiext";
    capability = "alpha";
    fn one(u32 x) -> u32;
}
extension beta : 2.0.1 {
    extends = "refrain:multiext";
    capability = "beta";
    fn two() -> void;
}
service multiext : 1.4.0 {
    side = both;
    thread = any;
    provides = [ "refrain:multiext" ];
    extensions = [ alpha, beta ];
    fn ping() -> u32;
}
)"};
        std::vector<refrain::schema::source_file> again;
        again.push_back(
            ridl_file("multiext.ridl", std::string{multiext_schema}));
        const auto second{
            refrain::schema::generate_from_sources(std::move(again))};
        expect(second.has_value());
        if (second) {
            expect(second->schema_hash == generated->schema_hash);
            for (const auto &file : generated->files) {
                expect(find_file(*second, file.path) != nullptr);
                if (const auto *other{find_file(*second, file.path)};
                    other != nullptr) {
                    expect(*other == file.content);
                }
            }
        }
    };

    "schema/generator emits query slots for two services independently"_test =
        [] {
            std::vector<refrain::schema::source_file> sources;
            sources.push_back(ridl_file("first.ridl",
                                        R"(module refrain::services::first;
version 1.0.0;
extension first_ext : 1.0.0 {
    extends = "refrain:first";
    capability = "first";
    fn one() -> u32;
}
service first : 1.0.0 {
    side = server;
    thread = any;
    provides = [ "refrain:first" ];
    extensions = [ first_ext ];
    fn ping() -> u32;
}
)"));
            sources.push_back(ridl_file("second.ridl",
                                        R"(module refrain::services::second;
version 1.0.0;
extension second_ext : 1.0.0 {
    extends = "refrain:second";
    capability = "second";
    fn two() -> u32;
}
service second : 1.0.0 {
    side = server;
    thread = any;
    provides = [ "refrain:second" ];
    extensions = [ second_ext ];
    fn ping() -> u32;
}
)"));
            const auto generated{
                refrain::schema::generate_from_sources(std::move(sources))};
            expect(generated.has_value());
            if (!generated) {
                return;
            }
            const auto *first{find_file(
                *generated, "c/refrain/contracts/services/first_abi.h")};
            const auto *second{find_file(
                *generated, "c/refrain/contracts/services/second_abi.h")};
            expect(first != nullptr);
            expect(second != nullptr);
            if (first != nullptr) {
                expect(first->find("refrain:first/first_ext") !=
                       std::string::npos);
                expect(first->find("(*query_extension)") != std::string::npos);
            }
            if (second != nullptr) {
                expect(second->find("refrain:second/second_ext") !=
                       std::string::npos);
                expect(second->find("(*query_extension)") != std::string::npos);
            }
        };

    // An extension may be a pure capability marker with an empty vtable body.
    "schema/generator accepts a method-less extension marker"_test = [] {
        std::vector<refrain::schema::source_file> sources;
        sources.push_back(ridl_file("marker.ridl",
                                    R"(module refrain::services::marker;
version 1.0.0;
extension marker_capability : 1.0.0 {
    extends = "refrain:marker";
    capability = "marker";
}
service marker : 1.0.0 {
    side = server;
    thread = any;
    provides = [ "refrain:marker" ];
    extensions = [ marker_capability ];
    fn ping() -> u32;
}
)"));
        const auto generated{
            refrain::schema::generate_from_sources(std::move(sources))};
        expect(generated.has_value());
        if (!generated) {
            return;
        }
        const auto *abi{
            find_file(*generated, "c/refrain/contracts/services/marker_abi.h")};
        expect(abi != nullptr);
        if (abi != nullptr) {
            expect(abi->find("refrain:marker/marker_capability") !=
                   std::string::npos);
            expect(abi->find("refrain_services_marker_marker_capability_"
                             "vtable") != std::string::npos);
            expect(abi->find("(*query_extension)") != std::string::npos);
        }
    };

    // An extension may live in an imported module; the service header only
    // needs the stable id string, while the extension module owns the vtable.
    "schema/generator supports extensions declared in an imported module"_test =
        [] {
            std::vector<refrain::schema::source_file> sources;
            sources.push_back(ridl_file("codec.ridl",
                                        R"(module refrain::ext;
version 1.0.0;
extension codec : 1.0.0 {
    extends = "refrain:hostc";
    capability = "codec";
    fn one(u32 x) -> u32;
}
)"));
            sources.push_back(ridl_file("hostc.ridl",
                                        R"(module refrain::hostc;
version 1.0.0;
import refrain::ext;
service hostc : 1.0.0 {
    side = server;
    thread = any;
    provides = [ "refrain:hostc" ];
    extensions = [ refrain::ext::codec ];
    fn ping() -> u32;
}
)"));
            const auto generated{
                refrain::schema::generate_from_sources(std::move(sources))};
            expect(generated.has_value());
            if (!generated) {
                return;
            }
            const auto *extension_abi{
                find_file(*generated, "c/refrain/contracts/ext_abi.h")};
            expect(extension_abi != nullptr);
            if (extension_abi != nullptr) {
                expect(extension_abi->find(
                           "REFRAIN_EXT_CODEC_ID \"refrain:hostc/codec\"") !=
                       std::string::npos);
                expect(extension_abi->find("typedef struct "
                                           "refrain_ext_codec_vtable") !=
                       std::string::npos);
            }
            const auto *service_abi{
                find_file(*generated, "c/refrain/contracts/hostc_abi.h")};
            expect(service_abi != nullptr);
            if (service_abi != nullptr) {
                expect(service_abi->find("(*query_extension)") !=
                       std::string::npos);
                expect(service_abi->find("refrain:hostc/codec") !=
                       std::string::npos);
            }
            const auto *service_cpp{
                find_file(*generated, "cpp/refrain/contracts/hostc.hpp")};
            expect(service_cpp != nullptr);
            if (service_cpp != nullptr) {
                expect(service_cpp->find("auto extension() const") !=
                       std::string::npos);
            }
        };

    // A service that does not declare extensions keeps byte-identical output
    // (apart from the banner hash) when another module starts using them.
    "schema/generator leaves non-extension services unchanged"_test = [] {
        constexpr std::string_view plain{R"(module refrain::services::plain;
version 1.0.0;
struct payload { u32 value; }
service plain : 1.0.0 {
    side = server;
    thread = any;
    provides = [ "refrain:plain" ];
    fn ping() -> u32;
}
)"};
        constexpr std::string_view extended{
            R"(module refrain::services::extended;
version 1.0.0;
extension codec : 1.0.0 {
    extends = "refrain:extended";
    capability = "codec";
    fn one() -> u32;
}
service extended : 1.0.0 {
    side = server;
    thread = any;
    provides = [ "refrain:extended" ];
    extensions = [ codec ];
    fn ping() -> u32;
}
)"};
        std::vector<refrain::schema::source_file> baseline;
        baseline.push_back(ridl_file("plain.ridl", std::string{plain}));
        const auto before{
            refrain::schema::generate_from_sources(std::move(baseline))};
        expect(before.has_value());
        std::vector<refrain::schema::source_file> with_extension;
        with_extension.push_back(ridl_file("plain.ridl", std::string{plain}));
        with_extension.push_back(
            ridl_file("extended.ridl", std::string{extended}));
        const auto after{
            refrain::schema::generate_from_sources(std::move(with_extension))};
        expect(after.has_value());
        if (!before || !after) {
            return;
        }
        // Aggregate/reflection headers legitimately list the new module; the
        // per-service headers for a service without extensions must not change.
        for (const auto &path :
             {std::string_view{"c/refrain/contracts/services/plain_abi.h"},
              std::string_view{"cpp/refrain/contracts/services/plain.hpp"}}) {
            const auto *before_file{find_file(*before, path)};
            const auto *after_file{find_file(*after, path)};
            expect(before_file != nullptr) << "missing baseline file: " << path;
            expect(after_file != nullptr) << "missing extended file: " << path;
            if (before_file == nullptr || after_file == nullptr) {
                continue;
            }
            expect(without_schema_hash(*after_file) ==
                   without_schema_hash(*before_file))
                << "output changed for: " << path;
        }
        // No extension declarations or query slots leak into the plain module.
        const auto *plain_abi{
            find_file(*after, "c/refrain/contracts/services/plain_abi.h")};
        expect(plain_abi != nullptr);
        if (plain_abi != nullptr) {
            expect(plain_abi->find("query_extension") == std::string::npos);
            expect(plain_abi->find("refrain_extension_ref") ==
                   std::string::npos);
        }
        const auto *plain_cpp{
            find_file(*after, "cpp/refrain/contracts/services/plain.hpp")};
        expect(plain_cpp != nullptr);
        if (plain_cpp != nullptr) {
            expect(plain_cpp->find("extension_registry") == std::string::npos);
            expect(plain_cpp->find("extension() const") == std::string::npos);
        }
    };
};

} // namespace
