// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <refrain/core/error.hpp>
#include <refrain/data/json.hpp>
#include <refrain/data/nbt.hpp>
#include <refrain/data/resource_location.hpp>
#include <refrain/data/vfs.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace refrain::data {

// Datapack content layer.
//
// Discovers the `data/` tree of a layered content stack (VFS priority order),
// parses the modelled content kinds, merges tags with vanilla semantics
// (`replace` / append across packs) and validates the result. Everything is a
// value: malformed JSON/NBT/layout fails `datapack_set::load()` with a
// `parse_error` naming the pack label and virtual path; semantic problems that
// should not kill boot (missing optional tag references, duplicate content ids
// across packs) are reported by `validate()`.
//
// Deliberate scope limits (documented, not errors):
//  - No Mojang registry cross-references yet: recipe item tags, loot item
//    names, worldgen biome/feature ids are only syntactically validated. The
//    block/item/entity registries are not modelled yet.
//  - Only the modelled kinds are stored (`recipe`, `loot_table`, `advancement`,
//    `worldgen`, `structure`); every other `data/<ns>/<kind>/...` file is
//    counted (and `*.json` parse-checked) through the `other` bucket so a
//    vanilla jar / mod pack with many kinds does not fail the load.
//  - Vanilla jar layout markers (`data/.mcassetsroot`), stray `pack.mcmeta`
//    files and nested datapacks (`data/<ns>/datapacks/<name>/...`) are not
//    content: they are counted as `ignored`/`nested` and never validated.
//  - `worldgen` follows vanilla directory-dispatch codecs: only the dispatch
//    subkinds (`feature`, `structure`, `carver`, `material_rule`,
//    `material_condition`, `block_state_provider`, `density_function`) carry a
//    top-level string `type`; the other subkinds (`biome`, `placed_feature`,
//    `noise`, ...) are dispatched by directory. A `density_function` document
//    may also be a JSON number or string constant (`zero.json` = `0.0`).
//  - `advancement` follows vanilla: it has no top-level "type" field (unlike
//    recipe/loot_table/worldgen), so validation checks its object shape
//    (`criteria` object) instead.
//  - Tags are keyed by (registry, tag id); registries may span several path
//    segments (`worldgen/biome`, `worldgen/structure`, ...) and are matched by
//    longest known prefix, everything else is a single-segment registry.
//  - Tags keep declaration order and duplicates (vanilla behaviour); no dedupe.
//  - Structure templates are decoded (gzip NBT) and kept as parsed values when
//    `parse_structures` is true; large jars pay that cost once at load.
//
// Thread safety: after `load()` the set is immutable; all queries are const and
// safe from multiple threads. `load()` itself is single-threaded.

/// Content kind modelled by the data layer.
enum class content_kind : std::uint8_t {
    recipe,
    loot_table,
    advancement,
    worldgen,
    structure,
    tags,
    other,
};

inline constexpr std::size_t content_kind_count = 7;

/// Directory name of a kind (`recipe`, `loot_table`, `advancement`, `worldgen`,
/// `structure`, `tags`, `other`).
[[nodiscard]] auto content_kind_name(content_kind kind) noexcept
    -> std::string_view;

/// One value entry of a merged tag: a plain content id or a `#tag` reference.
struct tag_value {
    resource_location id{};
    bool tag_reference{false};
    bool required{true};

    [[nodiscard]] friend auto operator==(const tag_value &, const tag_value &)
        -> bool = default;
};

/// One merged tag. `id` is the path relative to the tag-type directory
/// (e.g. `block` + `minecraft:logs`), `tag_type` is the registry directory
/// under `data/<ns>/tags/` (e.g. `block`, `item`, `worldgen/biome`).
struct tag_record {
    std::string tag_type{};
    resource_location id{};
    /// Merged values in pack priority order (low -> high). A `replace=true`
    /// pack clears everything accumulated so far before appending.
    std::vector<tag_value> values{};
    /// Number of packs that contributed (merged) to this tag.
    std::size_t sources{0};
    /// Virtual path of the last contributing file (highest priority); used by
    /// validation diagnostics so an error can point at the declaring file.
    std::string source_path{};

    [[nodiscard]] friend auto operator==(const tag_record &, const tag_record &)
        -> bool = default;
};

/// Immutable merged tag registry: lookup by (tag_type, id), iteration in
/// deterministic order. `tag_type` is the vanilla registry key the tag belongs
/// to; multi-segment registries keep their slash (`worldgen/biome`), tag ids
/// stay relative to the registry directory (e.g. `worldgen/biome` +
/// `minecraft:is_ocean`, or `block` + `minecraft:logs`).
class tag_set {
  public:
    tag_set() = default;

    /// Not `noexcept`: the lookup builds a temporary `std::string` key. A
    /// transparent lookup that removes the allocation is a separate
    /// optimization.
    [[nodiscard]] auto find(std::string_view tag_type,
                            const resource_location &id) const
        -> const tag_record *;

    [[nodiscard]] auto size() const noexcept -> std::size_t {
        return records_.size();
    }

    /// Distinct tag types, sorted.
    [[nodiscard]] auto tag_types() const -> std::vector<std::string>;

    /// All tags of one type, sorted by id.
    [[nodiscard]] auto tags_of(std::string_view tag_type) const
        -> std::vector<const tag_record *>;

    /// All tags, sorted by (tag_type, id).
    [[nodiscard]] auto all() const -> std::vector<const tag_record *>;

    /// Total number of merged values across all tags.
    [[nodiscard]] auto value_count() const noexcept -> std::size_t;

  private:
    friend class datapack_set;

    using key = std::pair<std::string, resource_location>;

    /// Loader-only constructor: adopts the merged records built by
    /// `datapack_set::load()` (not part of the public API).
    explicit tag_set(std::map<key, tag_record> records);

    std::map<key, tag_record> records_{};
};

/// Parsed NBT structure summary (kept instead of re-walking the value).
struct nbt_structure_info {
    std::size_t nodes{0};
    std::size_t depth{0};
};

/// One parsed content record.
struct content_entry {
    content_kind kind{content_kind::other};
    resource_location id{};
    /// Parsed JSON DOM for the JSON kinds; `null` for structures.
    json::value raw{};
    /// Parsed structure template (gzip NBT root compound) when the kind is
    /// `structure` and `parse_structures` was enabled.
    std::optional<nbt::value> structure{};
    nbt_structure_info structure_info{};
    /// Winning pack label and virtual path (diagnostics/override tracking).
    std::string source_label{};
    std::string source_path{};
    std::uint64_t byte_size{0};
};

/// Number of stored content entries per kind (after cross-pack overrides).
struct kind_counts {
    std::size_t recipe{0};
    std::size_t loot_table{0};
    std::size_t advancement{0};
    std::size_t worldgen{0};
    std::size_t structure{0};
    std::size_t tags{0};
    std::size_t other{0};

    [[nodiscard]] auto total() const noexcept -> std::size_t {
        return recipe + loot_table + advancement + worldgen + structure + tags +
               other;
    }
};

/// One mounted layer in priority order (low -> high).
struct layer_info {
    std::string label{};
    pack_format format{pack_format::directory};
    std::size_t file_count{0};
    bool has_manifest{false};
    /// Legacy `pack.pack_format`; 0 = absent.
    std::uint32_t manifest_pack_format{0};
    /// 26.3 `pack.min_format` / `pack.max_format` range; 0 = absent. The pair
    /// is authoritative when present (`pack_format` stays a legacy fallback).
    std::uint32_t manifest_min_format{0};
    std::uint32_t manifest_max_format{0};
    /// Jar-root `version.json` (`pack_version.data_major`) was used to satisfy
    /// an explicit `expected_pack_format` gate.
    bool has_version_metadata{false};
    std::uint32_t version_data_major{0};
};

/// Aggregated load statistics (`counts` = stored entries after overrides,
/// `parsed` = files parsed per kind before overrides collapse).
struct datapack_stats {
    kind_counts counts{};
    kind_counts parsed{};
    std::size_t total_files{0}; ///< files in every mounted layer
    std::size_t data_files{0};  ///< files under `data/` in every layer
    std::size_t tag_value_count{0};
    std::size_t override_count{0};
    std::size_t merged_tag_count{0}; ///< tags contributed by >1 layer
    std::size_t other_files{0};      ///< files classified into `other`
    /// Non-content entries that must not abort a vanilla jar load: layout
    /// markers (`data/.mcassetsroot`) and stray nested `pack.mcmeta` files.
    std::size_t ignored_files{0};
    /// Files below `data/<ns>/datapacks/<name>/` and the number of distinct
    /// nested datapacks. They are neither parsed nor semantically validated.
    std::size_t nested_datapack_files{0};
    std::size_t nested_datapacks{0};
    /// Unmodelled kind directories (`function`, `predicate`, ...) -> file
    /// count.
    std::map<std::string, std::size_t> other_kinds{};
    std::vector<layer_info> layers{};
};

/// Result of `datapack_set::validate()`.
struct validation_report {
    std::vector<std::string> errors{};
    std::vector<std::string> warnings{};
    datapack_stats stats{};

    [[nodiscard]] auto ok() const noexcept -> bool { return errors.empty(); }
};

/// Load options.
struct datapack_load_options {
    /// 0 = accept any format declaration; otherwise the layer must declare a
    /// matching format: `pack.mcmeta` legacy `pack_format` or the 26.3
    /// `pack.min_format`/`pack.max_format` range (target inside the range), or
    /// — when no manifest exists — the jar-root `version.json`
    /// `pack_version.data_major`. A layer that declares nothing fails with
    /// `version_mismatch` instead of silently passing.
    std::uint32_t expected_pack_format{0};
    /// Decode `data/<ns>/structure/**/*.nbt` templates (gzip NBT). When false
    /// the files are counted but not read.
    bool parse_structures{true};
};

/// Immutable parsed/validated datapack content (move-only; owns all values).
class datapack_set {
  public:
    datapack_set(datapack_set &&other) noexcept;
    auto operator=(datapack_set &&other) noexcept -> datapack_set &;
    ~datapack_set();

    datapack_set(const datapack_set &) = delete;
    auto operator=(const datapack_set &) -> datapack_set & = delete;

    /// Load a layered pack stack, given explicitly in priority order
    /// (low -> high). Tag merging and per-pack manifests need the individual
    /// layers; the `vfs` overload below delegates here through
    /// `vfs::packs()`.
    [[nodiscard]] static auto load(std::span<const pack *const> layers,
                                   datapack_load_options options = {})
        -> result<datapack_set>;

    /// Load a `data::vfs` snapshot. Every mounted layer participates in the
    /// merge in mount order (low -> high), so this has the same tag-merge and
    /// override semantics as the explicit-layer overload. An empty vfs is
    /// rejected with `invalid_argument` (mount at least one pack).
    [[nodiscard]] static auto load(const vfs &source,
                                   datapack_load_options options = {})
        -> result<datapack_set>;

    [[nodiscard]] auto stats() const noexcept -> const datapack_stats &;

    /// Stored entry counts per kind (same as `stats().counts`).
    [[nodiscard]] auto counts() const noexcept -> const kind_counts &;

    [[nodiscard]] auto tags() const noexcept -> const tag_set &;

    /// Stored content record for (kind, id), or nullptr.
    /// `content_kind::tags` is not stored here (see `tags()`).
    [[nodiscard]] auto find(content_kind kind,
                            const resource_location &id) const noexcept
        -> const content_entry *;

    /// All stored records of one kind, sorted by id.
    [[nodiscard]] auto entries(content_kind kind) const
        -> std::vector<const content_entry *>;

    /// Unmodelled kind directory -> file count, sorted by directory name.
    [[nodiscard]] auto other_counts() const noexcept
        -> const std::map<std::string, std::size_t> &;

    /// Duplicate content ids seen across layers, in layer-processing order
    /// (human-readable; `validate()` turns them into warnings).
    [[nodiscard]] auto duplicates() const noexcept
        -> const std::vector<std::string> &;

    [[nodiscard]] auto layer_count() const noexcept -> std::size_t;

    /// Semantic validation on top of the load-time checks (which already
    /// reject malformed JSON/NBT/layout and invalid `type` ids). Checks tag
    /// references (missing required -> error, optional -> warning), tag
    /// reference cycles, and reports cross-pack duplicates as warnings.
    [[nodiscard]] auto validate() const -> validation_report;

  private:
    datapack_set();

    struct impl;
    std::unique_ptr<impl> impl_;
};

} // namespace refrain::data
