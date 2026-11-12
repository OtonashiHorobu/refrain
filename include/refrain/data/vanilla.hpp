// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Vanilla content tables: the RVTB v1 container and its read-only view.
//
// World generation needs the code-resident registries (block states, biome
// parameters, registry listings) without parsing JSON at startup. The import
// step converts the game's datagen reports into one deterministic container;
// `vanilla_tables` maps it and answers lookups with no per-entry allocation.
//
// Layout: a 72-byte header, a section table of 16-byte records sorted by tag,
// then 8-byte aligned sections. All integers are little endian and fixed
// width: the container is mapped, not parsed. Strings live in one pool; a
// record that names one stores its pool index, and all other cross-references
// are section-relative indices.
//
// The container carries two identifiers for a block state on purpose: the dense
// `state` index assigned here (used by the generator and the storage codec) and
// the game's own `vanilla_id` (used by the protocol layer). They are not
// interchangeable.

#pragma once

#include <refrain/core/error.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace refrain::data {

/// Container format version understood by this build.
inline constexpr std::uint16_t vanilla_table_version = 1;

/// Sort key for the header's `kind` field.
inline constexpr std::uint16_t vanilla_table_kind = 1;

/// A state may carry at most this many properties. Import rejects anything
/// larger instead of truncating.
inline constexpr std::size_t max_state_properties = 8;

/// Upper bound on the string pool, in bytes.
inline constexpr std::size_t max_string_pool_bytes = 64U * 1024U * 1024U;

/// Stack size an item has when its registry data names none.
inline constexpr std::uint16_t default_item_stack_size = 64;

/// One state as supplied by the importer. `values` is aligned with the owning
/// block's property list and uses the same order.
struct state_spec {
    std::uint16_t vanilla_id{0};
    std::vector<std::string> values{};
    bool is_default{false};
};

/// One block as supplied by the importer.
struct block_spec {
    std::string name{};
    std::vector<std::string> property_names{};
    std::vector<std::vector<std::string>> property_values{};
    std::vector<state_spec> states{};
};

/// One biome climate row. Ranges are quantized with `(long)(v * 10000.0F)`,
/// matching the game's climate space.
struct biome_parameter_spec {
    std::string biome{};
    std::array<std::int32_t, 6> min{};
    std::array<std::int32_t, 6> max{};
    std::int32_t offset{0};
};

struct biome_preset_spec {
    std::string name{};
    std::vector<biome_parameter_spec> entries{};
};

struct registry_entry_spec {
    std::string name{};
    std::int32_t protocol_id{0};
};

struct registry_spec {
    std::string name{};
    std::vector<registry_entry_spec> entries{};
};

/// One item as supplied by the importer. The values come from the item's
/// default data components, which are the per-item registry data of the
/// target version.
struct item_spec {
    std::string name{};
    std::uint16_t max_stack_size{default_item_stack_size};
    std::uint32_t max_damage{0};
};

/// Everything the container stores.
struct vanilla_spec {
    std::int32_t data_version{0};
    std::vector<block_spec> blocks{};
    std::vector<biome_preset_spec> biome_presets{};
    std::vector<registry_spec> registries{};
    std::vector<item_spec> items{};
};

/// Serializes `spec` into an RVTB v1 file.
///
/// The builder canonicalizes ordering (blocks, properties, states, presets and
/// registries by name; preset entries keep their input order), so the same
/// semantic input always produces the same bytes. Fails with
/// `invalid_argument` when the specification is inconsistent (duplicate names,
/// a state whose value count differs from its block's property count, no or
/// more than one default state, more than `max_state_properties`, or a pool
/// over `max_string_pool_bytes`).
[[nodiscard]] auto build_vanilla_tables(const vanilla_spec &spec)
    -> result<std::vector<std::byte>>;

/// Header fields of a serialized container, for diagnostics and tests.
struct vanilla_table_info {
    std::uint16_t version{0};
    std::uint16_t kind{0};
    std::int32_t data_version{0};
    std::uint32_t strings{0};
    std::uint32_t blocks{0};
    std::uint32_t states{0};
    std::uint32_t properties{0};
    std::uint32_t presets{0};
    std::uint32_t registries{0};
    std::uint32_t items{0};
};

/// One block record.
struct block_view {
    std::string_view name{};
    std::uint32_t first_state{0};
    std::uint32_t state_count{0};
    std::uint32_t default_state{0};
};

/// One state record. `values` indexes the owning block's properties and has
/// exactly `values.size()` entries.
struct state_view {
    std::uint16_t block{0};
    std::uint16_t vanilla_id{0};
    std::string_view key{};
    std::span<const std::uint8_t> values{};
};

struct biome_parameter_view {
    std::array<std::int32_t, 6> min{};
    std::array<std::int32_t, 6> max{};
    std::int32_t offset{0};
    std::string_view biome{};
};

struct registry_entry_view {
    std::string_view name{};
    std::int32_t protocol_id{0};
};

/// One item record: the item key and the stack size and durability its
/// registry data declares. `max_damage` is zero for an item that cannot be
/// damaged.
struct item_view {
    std::string_view name{};
    std::uint16_t max_stack_size{0};
    std::uint32_t max_damage{0};
};

/// Read-only view over an RVTB v1 container.
///
/// The view borrows the bytes; they must outlive it. `open` validates the
/// header, every section boundary and every cross-reference, optionally checks
/// the BLAKE3 digest, and builds two open-addressing indexes (state key and
/// block name). Those indexes are the only allocation; lookups after that are
/// allocation-free.
class vanilla_tables {
  public:
    /// `verify` checks the BLAKE3 digest over the payload. Leave it on for
    /// untrusted files; tests that re-open a freshly built buffer may skip it.
    [[nodiscard]] static auto open(std::span<const std::byte> bytes,
                                   bool verify = true)
        -> result<vanilla_tables>;

    [[nodiscard]] auto info() const noexcept -> const vanilla_table_info & {
        return info_;
    }
    [[nodiscard]] auto data_version() const noexcept -> std::int32_t {
        return info_.data_version;
    }

    [[nodiscard]] auto string_count() const noexcept -> std::uint32_t {
        return info_.strings;
    }
    [[nodiscard]] auto block_count() const noexcept -> std::uint32_t {
        return info_.blocks;
    }
    [[nodiscard]] auto state_count() const noexcept -> std::uint32_t {
        return info_.states;
    }
    [[nodiscard]] auto property_count() const noexcept -> std::uint32_t {
        return info_.properties;
    }
    [[nodiscard]] auto preset_count() const noexcept -> std::uint32_t {
        return info_.presets;
    }
    [[nodiscard]] auto registry_count() const noexcept -> std::uint32_t {
        return info_.registries;
    }

    [[nodiscard]] auto block_at(std::uint16_t id) const -> block_view;
    [[nodiscard]] auto find_block(std::string_view name) const
        -> std::optional<std::uint16_t>;

    [[nodiscard]] auto state_at(std::uint32_t id) const -> state_view;
    [[nodiscard]] auto find_state(std::string_view key) const
        -> std::optional<std::uint32_t>;
    /// Looks a state up by the game's own state id (protocol use).
    [[nodiscard]] auto state_by_vanilla_id(std::uint16_t vanilla_id) const
        -> std::optional<std::uint32_t>;

    [[nodiscard]] auto property_name(std::uint16_t id) const
        -> std::string_view;
    [[nodiscard]] auto property_value_count(std::uint16_t id) const
        -> std::uint8_t;
    [[nodiscard]] auto property_value(std::uint16_t id,
                                      std::uint8_t index) const
        -> std::string_view;
    /// The property ids of one block, in the order the state values use.
    [[nodiscard]] auto block_properties(std::uint16_t block) const
        -> std::span<const std::uint8_t>;

    [[nodiscard]] auto preset_at(std::uint32_t index) const -> std::string_view;
    [[nodiscard]] auto find_preset(std::string_view name) const
        -> std::optional<std::uint32_t>;
    [[nodiscard]] auto biome_parameter_count(std::uint32_t preset) const
        -> std::uint32_t;
    [[nodiscard]] auto biome_parameter(std::uint32_t preset,
                                       std::uint32_t index) const
        -> biome_parameter_view;

    [[nodiscard]] auto registry_at(std::uint32_t index) const
        -> std::string_view;
    [[nodiscard]] auto find_registry(std::string_view name) const
        -> std::optional<std::uint32_t>;
    [[nodiscard]] auto registry_entry_count(std::uint32_t registry) const
        -> std::uint32_t;
    [[nodiscard]] auto registry_entry(std::uint32_t registry,
                                      std::uint32_t index) const
        -> registry_entry_view;

    /// Items in the container, sorted by name. The builder always writes the
    /// section, so a container whose source reports carried no item records
    /// still opens and answers every lookup with nothing.
    [[nodiscard]] auto item_count() const noexcept -> std::uint32_t {
        return info_.items;
    }
    [[nodiscard]] auto item_at(std::uint32_t index) const -> item_view;
    /// Binary search over the name-sorted item records.
    [[nodiscard]] auto find_item(std::string_view name) const
        -> std::optional<std::uint32_t>;

  private:
    struct section {
        std::uint32_t offset{0};
        std::uint32_t size{0};
    };

    [[nodiscard]] auto string_at(std::uint32_t index) const -> std::string_view;

    std::span<const std::byte> bytes_{};
    section strings_{};
    section blocks_{};
    section block_properties_{};
    section properties_{};
    section property_values_{};
    section states_{};
    section biomes_{};
    section registries_{};
    section items_{};
    section section_table_{};
    vanilla_table_info info_{};

    std::vector<std::uint32_t> block_index_{};
    std::vector<std::uint32_t> state_index_{};
    std::vector<std::pair<std::uint16_t, std::uint32_t>> vanilla_index_{};
};

/// In-memory inputs of an import. Tests construct these from synthetic JSON;
/// the file-based importer fills them from a datagen output directory.
struct vanilla_reports {
    std::string blocks_json{};
    std::vector<std::pair<std::string, std::string>> biome_presets{};
    std::string registries_json{};
    /// Item name and the `components/item/<name>.json` document that holds the
    /// item's default data components.
    std::vector<std::pair<std::string, std::string>> item_components{};
};

/// Converts the report documents into a container. Ordering and validation
/// follow `build_vanilla_tables`.
[[nodiscard]] auto build_vanilla_from_reports(const vanilla_reports &reports,
                                              std::int32_t data_version)
    -> result<std::vector<std::byte>>;

struct vanilla_import_options {
    std::string reports{}; ///< datagen `--reports` output directory
    std::string out{};     ///< target `content/vanilla/<version>`
    std::string game_version{"26.3"};
    std::int32_t data_version{5023};
    bool force{false};
};

struct vanilla_import_result {
    std::string tables_path{};
    std::string manifest_path{};
    std::uint64_t tables_bytes{0};
    std::string tables_digest{};
    std::uint32_t blocks{0};
    std::uint32_t states{0};
    std::uint32_t presets{0};
    std::uint32_t registries{0};
    std::uint32_t items{0};
};

/// Reads `blocks.json`, `biome_parameters/minecraft/*.json`,
/// `registries.json` and `minecraft/components/item/*.json` from
/// `options.reports`, writes `tables.rvtb` and
/// `MANIFEST.json` into `options.out`, and returns their hashes.
[[nodiscard]] auto import_vanilla_reports(const vanilla_import_options &options)
    -> result<vanilla_import_result>;

} // namespace refrain::data
