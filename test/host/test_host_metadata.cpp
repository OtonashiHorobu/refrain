// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// `refrain:host.metadata` tests over the generated schema metadata tables.

#include <refrain/host/metadata_api.hpp>

#include <boost/ut.hpp>

#include <cstdint>
#include <string_view>

namespace {

using boost::ut::expect;
using boost::ut::suite;
using namespace boost::ut::literals;

namespace host = refrain::host;
namespace meta = refrain::host::metadata;
namespace contracts = refrain::contracts;

const suite host_metadata_tests = [] {
    "host/metadata exposes types and fields"_test = [] {
        const auto api{host::make_metadata_provider()};
        expect(api.type_count() > 0U);

        const auto pos{api.type_find("refrain::core::chunk_pos")};
        expect(pos.has_value());
        if (pos) {
            expect(static_cast<meta::type_kind>(pos->kind) ==
                   meta::type_kind::structure);
            expect(pos->size == 12U);
            expect(pos->alignment == 4U);
            expect(pos->field_count == 2U);
            expect(pos->enum_count == 0U);
            expect(contracts::as_string_view(pos->id) ==
                   "refrain::core::chunk_pos");

            const auto x{api.field_at(contracts::as_string_view(pos->id), 0)};
            expect(x.has_value());
            if (x) {
                expect(contracts::as_string_view(x->name) == "x");
                expect(contracts::as_string_view(x->type_id) == "i32");
                expect(x->offset == 4U);
                expect(x->size == 4U);
            }
            const auto z{api.field_at(contracts::as_string_view(pos->id), 1)};
            expect(z.has_value());
            if (z) {
                expect(contracts::as_string_view(z->name) == "z");
                expect(z->offset == 8U);
            }
            const auto out_of_range{
                api.field_at(contracts::as_string_view(pos->id), 2)};
            expect(!out_of_range.has_value());
            if (!out_of_range) {
                expect(out_of_range.error() == meta::metadata_error::not_found);
            }
        }

        expect(!api.type_find("refrain::missing").has_value());
        expect(!api.type_at(99999).has_value());
    };

    "host/metadata exposes enum values and contracts"_test = [] {
        const auto api{host::make_metadata_provider()};
        const auto error{api.type_find("refrain::core::core_error")};
        expect(error.has_value());
        if (error) {
            expect(static_cast<meta::type_kind>(error->kind) ==
                   meta::type_kind::enumeration);
            expect(error->enum_count == 8U);
            const auto first{
                api.enum_value_at(contracts::as_string_view(error->id), 0)};
            expect(first.has_value());
            if (first) {
                expect(contracts::as_string_view(first->name) ==
                       "invalid_argument");
                expect(first->value == 1);
            }
            expect(!api.enum_value_at(contracts::as_string_view(error->id), 99)
                        .has_value());
        }

        expect(api.contract_count() > 0U);
        bool found_core{false};
        for (std::uint32_t index = 0; index < api.contract_count(); ++index) {
            const auto contract{api.contract_at(index)};
            expect(contract.has_value());
            if (!contract) {
                continue;
            }
            expect(!contracts::as_string_view(contract->schema_hash).empty());
            if (contracts::as_string_view(contract->id) ==
                "refrain:host.core") {
                found_core = true;
                expect(contract->abi_major == 1U);
                expect(contract->abi_minor == 0U);
                expect(contract->abi_patch == 0U);
                expect(contract->method_count > 0U);
            }
        }
        expect(found_core);
        expect(!api.schema_hash().empty());
    };
};

} // namespace
