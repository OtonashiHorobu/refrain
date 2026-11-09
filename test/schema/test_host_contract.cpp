// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Sealed host-contract schema checks: `composition = sealed`, the
// `opaque_ptr`/`mut_bytes` primitives and the reserved generated names.

#include <schema/schema.hpp>

#include <boost/ut.hpp>

#include <string>
#include <string_view>

namespace {

using namespace boost::ut;

namespace schema = refrain::schema;

[[nodiscard]] auto compile_ridl(std::string text)
    -> refrain::result<schema::generation> {
    return schema::generate_from_sources({schema::source_file{
        "test/host.ridl", schema::dialect::ridl, std::move(text)}});
}

[[nodiscard]] auto file_named(const schema::generation &generated,
                              std::string_view path) -> const std::string * {
    for (const auto &file : generated.files) {
        if (file.path == path) {
            return &file.content;
        }
    }
    return nullptr;
}

const suite host_contract_schema_tests = [] {
    "schema/sealed composition generates the sealed ABI constant"_test = [] {
        const auto generated{compile_ridl(R"(module refrain::demo;
version 1.0.0;

enum demo_error : u32 { failed = 1 }

struct version {
    u32 major;
    u32 minor;
    u32 patch;
}

service core : 1.0.0 {
    side = both;
    thread = any;
    composition = sealed;
    provides = [ "refrain:demo/core" ];
    capabilities = [ "log" ];
    fn version(out version value) -> void;
}
)")};
        expect(generated.has_value());
        if (!generated) {
            return;
        }
        const auto *abi{
            file_named(*generated, "c/refrain/contracts/demo_abi.h")};
        const auto *cpp{
            file_named(*generated, "cpp/refrain/contracts/demo.hpp")};
        expect(abi != nullptr);
        expect(cpp != nullptr);
        if (abi == nullptr || cpp == nullptr) {
            return;
        }
        expect(abi->find("#define REFRAIN_DEMO_CORE_COMPOSITION "
                         "REFRAIN_COMPOSITION_SEALED") != std::string::npos);
        expect(cpp->find("::refrain::contracts::composition_kind::sealed") !=
               std::string::npos);
        expect(cpp->find("class provider final") != std::string::npos);
    };

    "schema/opaque_ptr and mut_bytes generate C and C++ ABI types"_test = [] {
        const auto generated{compile_ridl(R"(module refrain::demo;
version 1.0.0;

enum demo_error : u32 { failed = 1 }

struct provider_ref {
    opaque_ptr vtable;
    opaque_ptr self;
    u32 abi_major;
}

service core : 1.0.0 {
    side = both;
    thread = any;
    composition = sealed;
    provides = [ "refrain:demo/core" ];
    fn query() -> result<provider_ref, demo_error>;
    fn alloc(u64 size) -> result<mut_bytes, demo_error>;
    fn free(mut_bytes block) -> void;
}
)")};
        expect(generated.has_value());
        if (!generated) {
            return;
        }
        const auto *abi{
            file_named(*generated, "c/refrain/contracts/demo_abi.h")};
        const auto *cpp{
            file_named(*generated, "cpp/refrain/contracts/demo.hpp")};
        expect(abi != nullptr);
        expect(cpp != nullptr);
        if (abi == nullptr || cpp == nullptr) {
            return;
        }
        expect(abi->find("void * vtable;") != std::string::npos);
        expect(abi->find("void * self;") != std::string::npos);
        expect(abi->find("refrain_mut_bytes *out_value") != std::string::npos);
        expect(abi->find("void (*free)(void *self, refrain_mut_bytes "
                         "block)") != std::string::npos);
        expect(cpp->find("offsetof(provider_ref, vtable) == 8") !=
               std::string::npos);
        expect(cpp->find("offsetof(provider_ref, self) == 16") !=
               std::string::npos);
        expect(cpp->find("offsetof(provider_ref, abi_major) == 24") !=
               std::string::npos);
        expect(cpp->find("sizeof(provider_ref) == 32") != std::string::npos);
        expect(cpp->find("std::span<std::uint8_t>") != std::string::npos);
        expect(cpp->find("::refrain::contracts::to_mut(block)") !=
               std::string::npos);
    };

    "schema/callback declarations generate C and C++ function pointers"_test =
        [] {
            const auto generated{compile_ridl(R"(module refrain::demo;
version 1.0.0;

callback job_callback = fn(opaque_ptr userdata, u64 value) -> void;

service core : 1.0.0 {
    side = both;
    thread = any;
    composition = sealed;
    provides = [ "refrain:demo/core" ];
    fn submit(job_callback callback, opaque_ptr userdata) -> u32;
}
)")};
            expect(generated.has_value());
            if (!generated) {
                return;
            }
            const auto *abi{
                file_named(*generated, "c/refrain/contracts/demo_abi.h")};
            const auto *cpp{
                file_named(*generated, "cpp/refrain/contracts/demo.hpp")};
            expect(abi != nullptr);
            expect(cpp != nullptr);
            if (abi == nullptr || cpp == nullptr) {
                return;
            }
            expect(abi->find("typedef void (*refrain_demo_job_callback)("
                             "void * userdata, uint64_t value);") !=
                   std::string::npos);
            expect(cpp->find("using job_callback = void (*)(void * userdata, "
                             "std::uint64_t value);") != std::string::npos);
            expect(abi->find("refrain_demo_job_callback callback") !=
                   std::string::npos);
        };

    "schema/callback restrictions are enforced"_test = [] {
        const auto non_void{compile_ridl(R"(module refrain::demo;
version 1.0.0;
callback bad = fn(opaque_ptr userdata) -> u32;
)")};
        expect(!non_void.has_value());
        expect(non_void.error().message.find("return values") !=
               std::string::npos);

        const auto bad_parameter{compile_ridl(R"(module refrain::demo;
version 1.0.0;
callback bad = fn(string text) -> void;
)")};
        expect(!bad_parameter.has_value());
        expect(bad_parameter.error().message.find("numeric primitive") !=
               std::string::npos);

        const auto struct_field{compile_ridl(R"(module refrain::demo;
version 1.0.0;
callback cb = fn(opaque_ptr userdata) -> void;
struct bad { cb callback; }
)")};
        expect(!struct_field.has_value());
        expect(struct_field.error().message.find("not allowed") !=
               std::string::npos);
    };

    "schema/reserved ABI member and parameter names are rejected"_test = [] {
        const auto field{compile_ridl(R"(module refrain::demo;
version 1.0.0;
struct bad { u32 struct_size; }
)")};
        expect(!field.has_value());
        expect(field.error().message.find(
                   "collides with a generated ABI member name") !=
               std::string::npos);

        const auto padding{compile_ridl(R"(module refrain::demo;
version 1.0.0;
struct bad { u32 reserved_0; }
)")};
        expect(!padding.has_value());
        expect(padding.error().message.find(
                   "collides with a generated ABI member name") !=
               std::string::npos);

        const auto parameter{compile_ridl(R"(module refrain::demo;
version 1.0.0;
enum demo_error : u32 { failed = 1 }
service core : 1.0.0 {
    side = both;
    thread = any;
    provides = [ "refrain:demo/core" ];
    fn call(u32 self) -> void;
}
)")};
        expect(!parameter.has_value());
        expect(parameter.error().message.find(
                   "reserved by the generated vtable") != std::string::npos);
    };

    "schema/event fields cannot use opaque_ptr or mut_bytes"_test = [] {
        const auto pointer{compile_ridl(R"(module refrain::demo;
version 1.0.0;
event changed { opaque_ptr ptr; }
)")};
        expect(!pointer.has_value());
        expect(pointer.error().message.find(
                   "cannot carry a pointer or writable buffer") !=
               std::string::npos);

        const auto buffer{compile_ridl(R"(module refrain::demo;
version 1.0.0;
event changed { mut_bytes data; }
)")};
        expect(!buffer.has_value());
        expect(buffer.error().message.find(
                   "cannot carry a pointer or writable buffer") !=
               std::string::npos);
    };
};

} // namespace
