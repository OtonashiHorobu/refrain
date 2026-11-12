// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

// `refrain:host.metadata` C ABI adapter over the generated metadata tables.

#include <refrain/host/metadata_api.hpp>

#include <refrain/contracts/abi.hpp>
#include <refrain/host/abi_bridge.hpp>
#include <refrain/metadata.hpp>

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace refrain::host {
namespace {

namespace abi = refrain::host::abi;
namespace meta = refrain::metadata;
namespace api = refrain::host::metadata;

[[nodiscard]] auto find_type(std::string_view id) -> const meta::type_entry * {
    for (const auto &type : meta::types) {
        if (type.id == id) {
            return &type;
        }
    }
    return nullptr;
}

void fill_type(const meta::type_entry &type, api::type_info &out) {
    out.struct_size = static_cast<std::uint32_t>(sizeof(out));
    out.id = abi::to_borrowed(type.id);
    out.kind = static_cast<refrain_host_metadata_type_kind>(type.kind);
    out.size = type.size;
    out.alignment = type.alignment;
    out.field_count = static_cast<std::uint32_t>(type.fields.size());
    out.enum_count = static_cast<std::uint32_t>(type.values.size());
}

auto host_type_count(void * /*self*/) -> std::uint32_t {
    return static_cast<std::uint32_t>(meta::types.size());
}

auto host_type_at(void * /*self*/, std::uint32_t index, api::type_info *out)
    -> refrain_status {
    if (out == nullptr) {
        return static_cast<refrain_status>(
            api::metadata_error::invalid_argument);
    }
    if (index >= meta::types.size()) {
        return static_cast<refrain_status>(api::metadata_error::not_found);
    }
    fill_type(meta::types[index], *out);
    return REFRAIN_STATUS_OK;
}

auto host_type_find(void * /*self*/, refrain_str id, api::type_info *out)
    -> refrain_status {
    if (out == nullptr) {
        return static_cast<refrain_status>(
            api::metadata_error::invalid_argument);
    }
    const auto *type{find_type(abi::to_view(id))};
    if (type == nullptr) {
        return static_cast<refrain_status>(api::metadata_error::not_found);
    }
    fill_type(*type, *out);
    return REFRAIN_STATUS_OK;
}

auto host_field_at(void * /*self*/, refrain_str type_id, std::uint32_t index,
                   api::field_info *out) -> refrain_status {
    if (out == nullptr) {
        return static_cast<refrain_status>(
            api::metadata_error::invalid_argument);
    }
    const auto *type{find_type(abi::to_view(type_id))};
    if (type == nullptr || index >= type->fields.size()) {
        return static_cast<refrain_status>(api::metadata_error::not_found);
    }
    const auto &field{type->fields[index]};
    out->struct_size = static_cast<std::uint32_t>(sizeof(*out));
    out->name = abi::to_borrowed(field.name);
    out->type_id = abi::to_borrowed(field.type_id);
    out->offset = field.offset;
    out->size = field.size;
    out->flags = field.flags;
    return REFRAIN_STATUS_OK;
}

auto host_enum_value_at(void * /*self*/, refrain_str type_id,
                        std::uint32_t index, api::enum_value_info *out)
    -> refrain_status {
    if (out == nullptr) {
        return static_cast<refrain_status>(
            api::metadata_error::invalid_argument);
    }
    const auto *type{find_type(abi::to_view(type_id))};
    if (type == nullptr || index >= type->values.size()) {
        return static_cast<refrain_status>(api::metadata_error::not_found);
    }
    const auto &value{type->values[index]};
    out->struct_size = static_cast<std::uint32_t>(sizeof(*out));
    out->name = abi::to_borrowed(value.name);
    out->value = value.value;
    return REFRAIN_STATUS_OK;
}

auto host_contract_count(void * /*self*/) -> std::uint32_t {
    return static_cast<std::uint32_t>(meta::contracts.size());
}

auto host_contract_at(void * /*self*/, std::uint32_t index,
                      api::contract_info *out) -> refrain_status {
    if (out == nullptr) {
        return static_cast<refrain_status>(
            api::metadata_error::invalid_argument);
    }
    if (index >= meta::contracts.size()) {
        return static_cast<refrain_status>(api::metadata_error::not_found);
    }
    const auto &contract{meta::contracts[index]};
    out->struct_size = static_cast<std::uint32_t>(sizeof(*out));
    out->id = abi::to_borrowed(contract.id);
    out->abi_major = contract.abi_major;
    out->abi_minor = contract.abi_minor;
    out->abi_patch = contract.abi_patch;
    out->schema_hash = abi::to_borrowed(contract.schema_hash);
    out->method_count = contract.method_count;
    return REFRAIN_STATUS_OK;
}

auto host_schema_hash(void * /*self*/) -> refrain_str {
    return abi::to_borrowed(meta::schema_hash);
}

} // namespace

auto metadata_vtable() noexcept -> const refrain_host_metadata_vtable & {
    static const refrain_host_metadata_vtable table{
        .struct_size = REFRAIN_HOST_METADATA_VTABLE_SIZE,
        .abi_major = REFRAIN_HOST_METADATA_ABI_MAJOR,
        .abi_minor = REFRAIN_HOST_METADATA_ABI_MINOR,
        .abi_patch = REFRAIN_HOST_METADATA_ABI_PATCH,
        .type_count = &host_type_count,
        .type_at = &host_type_at,
        .type_find = &host_type_find,
        .field_at = &host_field_at,
        .enum_value_at = &host_enum_value_at,
        .contract_count = &host_contract_count,
        .contract_at = &host_contract_at,
        .schema_hash = &host_schema_hash,
    };
    return table;
}

} // namespace refrain::host
