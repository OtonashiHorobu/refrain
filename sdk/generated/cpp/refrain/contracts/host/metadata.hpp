// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// GENERATED FILE - DO NOT EDIT.
// Source of truth: RSC/RIDL schemas (schema/, sdk/idl/).
// Regenerate with: refrain-tools schema gen
// Schema format: v1
// Schema hash: e168f1954a0feabb

#ifndef REFRAIN_GENERATED_CPP_REFRAIN_CONTRACTS_HOST_METADATA_HPP
#define REFRAIN_GENERATED_CPP_REFRAIN_CONTRACTS_HOST_METADATA_HPP

#include <refrain/contracts/host/metadata_abi.h>
#include <refrain/contracts/abi.hpp>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <refrain/core/error.hpp>
#include <refrain/core/schedule.hpp>

namespace refrain::host::metadata {

enum class metadata_error : std::uint32_t { // enum 'metadata_error'
    invalid_argument = 1,
    not_found = 2,
    internal = 3,
};

enum class type_kind : std::uint32_t { // enum 'type_kind'
    structure = 0,
    enumeration = 1,
    flags = 2,
    record = 3,
    packet = 4,
};

using type_info = ::refrain_host_metadata_type_info;
/// Zero-initialized `type_info` with the mandatory `struct_size` set; fill the fields before passing it across the ABI.
[[nodiscard]] inline auto make_type_info() noexcept -> type_info {
    type_info value{};
    ::refrain::contracts::init_struct(value);
    return value;
}
using field_info = ::refrain_host_metadata_field_info;
/// Zero-initialized `field_info` with the mandatory `struct_size` set; fill the fields before passing it across the ABI.
[[nodiscard]] inline auto make_field_info() noexcept -> field_info {
    field_info value{};
    ::refrain::contracts::init_struct(value);
    return value;
}
using enum_value_info = ::refrain_host_metadata_enum_value_info;
/// Zero-initialized `enum_value_info` with the mandatory `struct_size` set; fill the fields before passing it across the ABI.
[[nodiscard]] inline auto make_enum_value_info() noexcept -> enum_value_info {
    enum_value_info value{};
    ::refrain::contracts::init_struct(value);
    return value;
}
using contract_info = ::refrain_host_metadata_contract_info;
/// Zero-initialized `contract_info` with the mandatory `struct_size` set; fill the fields before passing it across the ABI.
[[nodiscard]] inline auto make_contract_info() noexcept -> contract_info {
    contract_info value{};
    ::refrain::contracts::init_struct(value);
    return value;
}

static_assert(sizeof(type_info) == 48);
static_assert(alignof(type_info) == 8);
static_assert(offsetof(type_info, struct_size) == 0);
static_assert(offsetof(type_info, id) == 8);
static_assert(offsetof(type_info, kind) == 24);
static_assert(offsetof(type_info, size) == 28);
static_assert(offsetof(type_info, alignment) == 32);
static_assert(offsetof(type_info, field_count) == 36);
static_assert(offsetof(type_info, enum_count) == 40);
static_assert(sizeof(field_info) == 56);
static_assert(alignof(field_info) == 8);
static_assert(offsetof(field_info, struct_size) == 0);
static_assert(offsetof(field_info, name) == 8);
static_assert(offsetof(field_info, type_id) == 24);
static_assert(offsetof(field_info, offset) == 40);
static_assert(offsetof(field_info, size) == 44);
static_assert(offsetof(field_info, flags) == 48);
static_assert(sizeof(enum_value_info) == 32);
static_assert(alignof(enum_value_info) == 8);
static_assert(offsetof(enum_value_info, struct_size) == 0);
static_assert(offsetof(enum_value_info, name) == 8);
static_assert(offsetof(enum_value_info, value) == 24);
static_assert(sizeof(contract_info) == 64);
static_assert(alignof(contract_info) == 8);
static_assert(offsetof(contract_info, struct_size) == 0);
static_assert(offsetof(contract_info, id) == 8);
static_assert(offsetof(contract_info, abi_major) == 24);
static_assert(offsetof(contract_info, abi_minor) == 28);
static_assert(offsetof(contract_info, abi_patch) == 32);
static_assert(offsetof(contract_info, schema_hash) == 40);
static_assert(offsetof(contract_info, method_count) == 56);

inline constexpr std::string_view service_id = "refrain:host.metadata";
inline constexpr std::uint32_t abi_major = 1;
inline constexpr std::uint32_t abi_minor = 0;
inline constexpr std::uint32_t abi_patch = 0;
inline constexpr ::refrain::contracts::side_kind side = ::refrain::contracts::side_kind::both;
inline constexpr ::refrain::contracts::thread_kind thread = ::refrain::contracts::thread_kind::any;
inline constexpr ::refrain::contracts::composition_kind composition = ::refrain::contracts::composition_kind::sealed;
inline constexpr std::string_view capabilities = "types,contracts";
inline constexpr std::string_view required_services = "refrain:core>=1.0.0";
inline constexpr std::string_view service_ids[] = {"refrain:host.metadata"};

/// Type-safe wrapper around `refrain_host_metadata_vtable`. The provider (host) owns the table and `self`; this class is a borrowed call surface.
class provider final {
  public:
    provider(const ::refrain_host_metadata_vtable &table, void *self) noexcept
        : table_{&table}, self_{self} {}

    provider(const provider &) = delete;
    auto operator=(const provider &) -> provider & = delete;

    [[nodiscard]] auto table() const noexcept
        -> const ::refrain_host_metadata_vtable & {
        return *table_;
    }

    [[nodiscard]] auto self() const noexcept -> void * {
        return self_;
    }

    [[nodiscard]] auto type_count() const -> std::uint32_t {
        return static_cast<std::uint32_t>(table_->type_count(self_));
    }

    [[nodiscard]] auto type_at(std::uint32_t index) const -> std::expected<::refrain::host::metadata::type_info, ::refrain::host::metadata::metadata_error> {
        ::refrain_host_metadata_type_info value{};
        ::refrain::contracts::init_struct(value);
        if (const auto code = table_->type_at(self_, index, &value); code != REFRAIN_STATUS_OK) {
            return std::unexpected(static_cast<::refrain::host::metadata::metadata_error>(code));
        }
        return value;
    }

    [[nodiscard]] auto type_find(std::string_view id) const -> std::expected<::refrain::host::metadata::type_info, ::refrain::host::metadata::metadata_error> {
        ::refrain_host_metadata_type_info value{};
        ::refrain::contracts::init_struct(value);
        if (const auto code = table_->type_find(self_, ::refrain::contracts::to_borrowed(id), &value); code != REFRAIN_STATUS_OK) {
            return std::unexpected(static_cast<::refrain::host::metadata::metadata_error>(code));
        }
        return value;
    }

    [[nodiscard]] auto field_at(std::string_view type_id, std::uint32_t index) const -> std::expected<::refrain::host::metadata::field_info, ::refrain::host::metadata::metadata_error> {
        ::refrain_host_metadata_field_info value{};
        ::refrain::contracts::init_struct(value);
        if (const auto code = table_->field_at(self_, ::refrain::contracts::to_borrowed(type_id), index, &value); code != REFRAIN_STATUS_OK) {
            return std::unexpected(static_cast<::refrain::host::metadata::metadata_error>(code));
        }
        return value;
    }

    [[nodiscard]] auto enum_value_at(std::string_view type_id, std::uint32_t index) const -> std::expected<::refrain::host::metadata::enum_value_info, ::refrain::host::metadata::metadata_error> {
        ::refrain_host_metadata_enum_value_info value{};
        ::refrain::contracts::init_struct(value);
        if (const auto code = table_->enum_value_at(self_, ::refrain::contracts::to_borrowed(type_id), index, &value); code != REFRAIN_STATUS_OK) {
            return std::unexpected(static_cast<::refrain::host::metadata::metadata_error>(code));
        }
        return value;
    }

    [[nodiscard]] auto contract_count() const -> std::uint32_t {
        return static_cast<std::uint32_t>(table_->contract_count(self_));
    }

    [[nodiscard]] auto contract_at(std::uint32_t index) const -> std::expected<::refrain::host::metadata::contract_info, ::refrain::host::metadata::metadata_error> {
        ::refrain_host_metadata_contract_info value{};
        ::refrain::contracts::init_struct(value);
        if (const auto code = table_->contract_at(self_, index, &value); code != REFRAIN_STATUS_OK) {
            return std::unexpected(static_cast<::refrain::host::metadata::metadata_error>(code));
        }
        return value;
    }

    [[nodiscard]] auto schema_hash() const -> std::string_view {
        return ::refrain::contracts::as_string_view(table_->schema_hash(self_));
    }

  private:
    const ::refrain_host_metadata_vtable *table_;
    void *self_;
};

inline constexpr std::array<::refrain::core::schedule::task_descriptor, 0>
    schedule_tasks{};

} // namespace refrain::host::metadata

#endif // REFRAIN_GENERATED_CPP_REFRAIN_CONTRACTS_HOST_METADATA_HPP
