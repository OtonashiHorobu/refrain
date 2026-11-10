// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// `refrain:host.core` adapter tests. A fake host context exercises the
// generated C ABI table and the C++ wrapper, including error mapping and the
// null/struct_size guards that must not let a bad caller cross the boundary.

#include <refrain/app/host_context.hpp>
#include <refrain/host/core_api.hpp>
#include <refrain/mod/host.hpp>

#include <boost/ut.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

// `using namespace boost::ut;` would import ut::override and collide with the
// virt-specifier used by the fake context overrides.
using boost::ut::expect;
using boost::ut::suite;
using namespace boost::ut::literals;

namespace host = refrain::host;
namespace core = refrain::host::core;

class fake_context final : public host::context {
  public:
    void log(std::uint32_t level, std::string_view message) noexcept override {
        last_level = level;
        last_message = message;
    }

    [[nodiscard]] auto now_ns() const noexcept -> std::int64_t override {
        return 123;
    }

    [[nodiscard]] auto tick_now() const noexcept -> std::int64_t override {
        return 7;
    }

    void version(core::host_version &out) const noexcept override {
        out.major = 1;
        out.minor = 2;
        out.patch = 3;
    }

    [[nodiscard]] auto config_i64(std::string_view ns, std::string_view key)
        -> refrain::result<std::int64_t> override {
        last_config_namespace = ns;
        if (key == "missing") {
            return std::unexpected{refrain::make_error(
                refrain::error_code::not_found, "no such key")};
        }
        return 42;
    }

    [[nodiscard]] auto config_bool(std::string_view ns, std::string_view key)
        -> refrain::result<bool> override {
        last_config_namespace = ns;
        if (key == "missing") {
            return std::unexpected{refrain::make_error(
                refrain::error_code::not_found, "no such key")};
        }
        return true;
    }

    [[nodiscard]] auto config_string(std::string_view ns, std::string_view key)
        -> refrain::result<std::string_view> override {
        last_config_namespace = ns;
        if (key == "missing") {
            return std::unexpected{refrain::make_error(
                refrain::error_code::not_found, "no such key")};
        }
        return std::string_view{"hello"};
    }

    [[nodiscard]] auto query_service(std::string_view service,
                                     std::string_view instance)
        -> refrain::result<core::provider_ref> override {
        if (service.empty()) {
            return std::unexpected{refrain::make_error(
                refrain::error_code::invalid_argument, "empty service")};
        }
        queried_service = service;
        queried_instance = instance;
        core::provider_ref ref{};
        ref.vtable = &some_vtable;
        ref.self = this;
        ref.abi_major = 1;
        return ref;
    }

    [[nodiscard]] auto register_provider(std::string_view service,
                                         std::string_view instance,
                                         const core::provider_ref &ref)
        -> refrain::status override {
        if (service == "sealed") {
            return std::unexpected{refrain::make_error(
                refrain::error_code::permission_denied, "sealed service")};
        }
        registered_service = service;
        registered_instance = instance;
        registered_ref = ref;
        return refrain::status_ok();
    }

    [[nodiscard]] auto alloc(std::uint64_t size, std::uint64_t alignment)
        -> refrain::result<std::span<std::uint8_t>> override {
        if (size == 0) {
            return std::unexpected{refrain::make_error(
                refrain::error_code::invalid_argument, "zero size")};
        }
        if (size == 999) {
            return std::unexpected{refrain::make_error(
                refrain::error_code::out_of_memory, "simulated OOM")};
        }
        allocation.assign(static_cast<std::size_t>(size), 0xABU);
        allocated_alignment = alignment;
        return std::span<std::uint8_t>{allocation};
    }

    void free(std::span<std::uint8_t> block) noexcept override {
        freed_bytes += block.size();
    }

    std::uint32_t last_level{0};
    std::string last_message{};
    std::string last_config_namespace{};
    std::string queried_service{};
    std::string queried_instance{};
    std::string registered_service{};
    std::string registered_instance{};
    core::provider_ref registered_ref{};
    std::uint64_t allocated_alignment{0};
    std::uint64_t freed_bytes{0};
    std::vector<std::uint8_t> allocation{};
    int some_vtable{0};
};

const suite host_core_tests = [] {
    "host/core version/log/time cross the generated table"_test = [] {
        fake_context fake;
        const auto api{host::make_core_provider(fake)};

        core::host_version version = core::make_host_version();
        api.version(version);
        expect(version.major == 1U);
        expect(version.minor == 2U);
        expect(version.patch == 3U);
        expect(version.struct_size == REFRAIN_HOST_CORE_HOST_VERSION_SIZE);

        api.log(2, "hello");
        expect(fake.last_level == 2U);
        expect(fake.last_message == "hello");

        expect(api.now_ns() == std::int64_t{123});
        expect(api.tick_now() == std::int64_t{7});
    };

    "host/core config getters map errors and borrowed strings"_test = [] {
        fake_context fake;
        const auto api{host::make_core_provider(fake)};

        expect(api.config_get_i64("demo", "answer").value() ==
               std::int64_t{42});
        expect(api.config_get_bool("demo", "flag").value());
        expect(fake.last_config_namespace == "demo");

        const auto text{api.config_get_string("demo", "name")};
        expect(text.has_value());
        expect(*text == "hello");

        const auto missing{api.config_get_i64("demo", "missing")};
        expect(!missing.has_value());
        expect(missing.error() == core::host_error::not_found);

        const auto message{api.last_error_message()};
        expect(message.has_value());
        expect(*message == "no such key");
    };

    "host/core query and register provider"_test = [] {
        fake_context fake;
        const auto api{host::make_core_provider(fake)};

        const auto found{api.query_service("refrain:world", {})};
        expect(found.has_value());
        expect(found->vtable == &fake.some_vtable);
        expect(found->self == &fake);
        expect(found->abi_major == 1U);
        expect(fake.queried_service == "refrain:world");

        core::provider_ref ref{};
        ref.vtable = &fake.some_vtable;
        ref.self = &fake;
        ref.abi_major = 1;
        expect(api.register_provider("refrain:world", {}, ref).has_value());
        expect(fake.registered_service == "refrain:world");
        expect(fake.registered_ref.vtable == ref.vtable);

        const auto sealed{api.register_provider("sealed", {}, ref)};
        expect(!sealed.has_value());
        expect(sealed.error() == core::host_error::permission_denied);
        const auto message{api.last_error_message()};
        expect(message.has_value());
        expect(*message == "sealed service");
    };

    "host/core alloc/free round trip and error mapping"_test = [] {
        fake_context fake;
        const auto api{host::make_core_provider(fake)};

        const auto block{api.alloc(16, 8)};
        expect(block.has_value());
        expect(block->size() == 16U);
        expect(fake.allocated_alignment == 8U);
        expect((*block)[0] == std::uint8_t{0xAB});

        api.free(*block);
        expect(fake.freed_bytes == 16U);

        const auto zero{api.alloc(0, 8)};
        expect(!zero.has_value());
        expect(zero.error() == core::host_error::invalid_argument);

        const auto oom{api.alloc(999, 8)};
        expect(!oom.has_value());
        expect(oom.error() == core::host_error::out_of_memory);
        const auto message{api.last_error_message()};
        expect(message.has_value());
        expect(*message == "simulated OOM");
    };

    "host/core sealed provider registration is host-owned"_test = [] {
        struct sealed_vtable {
            std::uint32_t struct_size{16};
            std::uint32_t abi_major{1};
            std::uint32_t abi_minor{0};
            std::uint32_t abi_patch{0};
        };
        sealed_vtable table{};
        int self{0};

        refrain::mod::host owner{};
        const refrain::mod::provider_api api{
            .vtable = &table, .self = &self, .abi_major = 1};
        expect(owner.register_sealed_provider("refrain:host.core", api, "1.0.0")
                   .has_value());
        expect(owner.is_sealed_service("refrain:host.core"));
        expect(!owner.is_sealed_service("refrain:world"));

        const auto active{
            owner.services().active_provider("refrain:host.core")};
        expect(active.has_value());
        if (active) {
            expect(active->id == "host");
            expect(active->version == "1.0.0");
            expect(active->source == "host");
            expect(active->instance != nullptr);
        }
    };

    "host/core app context serves the sealed provider and host memory"_test =
        [] {
            refrain::mod::host owner{};
            refrain::app::host_context context{owner};
            const refrain::mod::provider_api api{
                .vtable = &refrain::host::core_vtable(),
                .self = &context,
                .abi_major = REFRAIN_HOST_CORE_ABI_MAJOR,
            };
            expect(static_cast<bool>(owner.register_sealed_provider(
                "refrain:host.core", api, "1.0.0")));

            const auto found{context.query_service("refrain:host.core", {})};
            expect(found.has_value());
            if (found) {
                expect(found->vtable == static_cast<const void *>(
                                            &refrain::host::core_vtable()));
                expect(found->self == &context);
            }

            const auto block{context.alloc(32, 16)};
            expect(block.has_value());
            if (block) {
                expect(block->size() == 32U);
                context.free(*block);
            }

            const auto missing{context.query_service("refrain:missing", {})};
            expect(!missing.has_value());
        };

    "host/core app context reads mounted config namespaces"_test = [] {
        refrain::core::config::store config;
        expect(static_cast<bool>(config.mount_text(
            "demo", "answer = 42\nflag = true\nname = \"hello\"\n", "<demo>")));

        refrain::mod::host owner{};
        refrain::app::host_context context{owner, {}, &config};
        expect(context.config_i64("demo", "answer").value() ==
               std::int64_t{42});
        expect(context.config_bool("demo", "flag").value());
        const auto name{context.config_string("demo", "name")};
        expect(name.has_value());
        expect(*name == "hello");

        const auto missing_key{context.config_i64("demo", "missing")};
        expect(!missing_key.has_value());
        expect(missing_key.error().code == refrain::error_code::not_found);
        const auto missing_ns{context.config_i64("missing", "answer")};
        expect(!missing_ns.has_value());
        expect(missing_ns.error().code == refrain::error_code::not_found);

        refrain::app::host_context no_config{owner};
        const auto disabled{no_config.config_i64("demo", "answer")};
        expect(!disabled.has_value());
        expect(disabled.error().code == refrain::error_code::unsupported);
    };

    "host/core C ABI guards null and struct_size"_test = [] {
        fake_context fake;
        const auto &table{host::core_vtable()};
        expect(table.struct_size == REFRAIN_HOST_CORE_VTABLE_SIZE);
        expect(table.abi_major == REFRAIN_HOST_CORE_ABI_MAJOR);

        std::int64_t value{0};
        refrain_str key{};
        expect(table.config_get_i64(nullptr, key, key, &value) ==
               static_cast<refrain_status>(core::host_error::invalid_argument));
        expect(table.config_get_i64(&fake, key, key, nullptr) ==
               static_cast<refrain_status>(core::host_error::invalid_argument));

        const auto null_query{table.query_service(&fake, {}, {}, nullptr)};
        expect(null_query ==
               static_cast<refrain_status>(core::host_error::invalid_argument));

        core::provider_ref ref{};
        table.register_provider(&fake, {}, {}, &ref);
        expect(table.register_provider(&fake, {}, {}, nullptr) ==
               static_cast<refrain_status>(core::host_error::invalid_argument));
        expect(table.register_provider(&fake, {}, {}, &ref) ==
               static_cast<refrain_status>(core::host_error::version_mismatch));
    };
};

} // namespace
