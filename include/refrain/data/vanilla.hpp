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

/// Tool family of a mining tool. The numeric value is the bit position a
/// block's tool mask and direct-correct mask use. `none` marks an item that
/// carries a tool component without mining rules.
enum class tool_type : std::uint8_t {
    pickaxe = 0,
    axe = 1,
    shovel = 2,
    hoe = 3,
    shears = 4,
    sword = 5,
    none = 0xFF,
};

/// Bit of `type` in a block tool mask. `none` has no bit.
[[nodiscard]] constexpr auto tool_type_bit(tool_type type) -> std::uint8_t {
    return type == tool_type::none ? 0U
                                   : static_cast<std::uint8_t>(
                                         1U << static_cast<std::uint8_t>(type));
}

/// Mask of every tool-family bit.
inline constexpr std::uint8_t all_tool_type_bits =
    static_cast<std::uint8_t>(tool_type_bit(tool_type::sword) << 1U) - 1U;

/// Tool material of a tiered tool. `none` marks shears, swords and items
/// without tiered rules.
enum class tool_material : std::uint8_t {
    none = 0,
    wood = 1,
    stone = 2,
    copper = 3,
    iron = 4,
    diamond = 5,
    netherite = 6,
    gold = 7,
};

/// Harvest level an entry without tiered rules carries.
inline constexpr std::uint8_t harvest_level_none = 0xFF;

/// Highest tool-material harvest level the table stores.
inline constexpr std::uint8_t max_harvest_level = 3;

/// The block requires a correct tool for drops.
inline constexpr std::uint8_t block_mining_requires_correct_tool = 1U;
/// The block is air.
inline constexpr std::uint8_t block_mining_air = 2U;
/// The block is a liquid block.
inline constexpr std::uint8_t block_mining_fluid = 4U;

/// Mining behaviour of one block.
///
/// The report compresses the reference's tool rules: a tiered family mines at
/// one speed and yields drops down to one harvest threshold, while shears and
/// swords carry their speed and drop answer on the block side. The extractor
/// proves the compression against the game for every tool and block pair.
struct block_mining_spec {
    /// Destroy time the reference caches on every state of the block; `-1`
    /// means unbreakable.
    float hardness{0.0F};
    /// Mining speed of shears on the block.
    float shears_speed{1.0F};
    /// Mining speed of a sword on the block.
    float sword_speed{1.0F};
    /// Bits of `block_mining_requires_correct_tool`, `block_mining_air` and
    /// `block_mining_fluid`.
    std::uint8_t flags{0};
    /// Tool families that mine the block efficiently, as `tool_type` bits.
    std::uint8_t tool_mask{0};
    /// Lowest tool-material harvest level that yields drops.
    std::uint8_t needed_tier{0};
    /// Tool families that yield drops without a tier threshold, as
    /// `tool_type` bits; only shears and swords can appear.
    std::uint8_t direct_correct{0};
};

/// One item that carries a tool component.
struct tool_mining_spec {
    std::string name{};
    tool_type type{tool_type::none};
    tool_material material{tool_material::none};
    /// Harvest level of `material`, or `harvest_level_none` for an entry
    /// without tiered rules.
    std::uint8_t harvest_level{harvest_level_none};
    /// Mining speed of a tiered family on the blocks it mines efficiently.
    float tier_speed{1.0F};
    float default_mining_speed{1.0F};
    std::uint16_t damage_per_block{0};
    std::uint16_t max_stack_size{default_item_stack_size};
    std::uint32_t max_damage{0};
    bool can_destroy_blocks_in_creative{true};
};

/// Highest light level a state can emit or dampen.
inline constexpr std::uint8_t max_light_level = 15;

/// Index that means "this state has no light occlusion shape".
inline constexpr std::uint32_t no_light_shape = 0xFFFFFFFFU;

/// Face order of a light shape: down, up, north, south, west, east.
inline constexpr std::size_t light_shape_faces = 6;

/// Upper bound on the boxes one face of a light shape may carry. The report
/// importer rejects anything larger instead of truncating.
inline constexpr std::size_t max_light_boxes_per_face = 16;

/// The six face shapes the lighting engine uses for shape-based occlusion.
///
/// Each face is a list of boxes `{min_x, min_y, min_z, max_x, max_y, max_z}`
/// in block units. A shape may extend beyond its block cell (a piston head's
/// arm does), so the bounds are not clamped to the unit block; the lighting
/// engine clips a face to the block when it tests coverage.
struct light_shape_spec {
    std::array<std::vector<std::array<double, 6>>, light_shape_faces> faces{};
};

/// Light behaviour of one block state.
struct block_light_spec {
    /// The game's own state id, the same id the block list report carries.
    std::uint16_t vanilla_id{0};
    /// Block light level the state emits, 0..15.
    std::uint8_t emission{0};
    /// Light levels a path loses when it enters the state, 0..15.
    std::uint8_t dampening{15};
    /// Whether the state occludes light with a shape.
    bool can_occlude{true};
    /// Whether the lighting engine uses the state's shape for occlusion.
    bool use_shape_for_light_occlusion{false};
    /// Index into `vanilla_spec::light_shapes` or `no_light_shape`.
    std::uint32_t shape{no_light_shape};
};

/// Everything the container stores.
struct vanilla_spec {
    std::int32_t data_version{0};
    std::vector<block_spec> blocks{};
    std::vector<biome_preset_spec> biome_presets{};
    std::vector<registry_spec> registries{};
    std::vector<item_spec> items{};
    /// Per-state light records and their occlusion shapes. Both are empty when
    /// the source reports carry no light data; the container then omits the
    /// light sections and the runtime falls back to the documented defaults.
    std::vector<light_shape_spec> light_shapes{};
    std::vector<block_light_spec> block_lights{};
    /// Per-block mining records, aligned with `blocks`, and the tool table.
    /// Both are empty when the source reports carry no mining data; the
    /// container then omits the mining sections and the runtime falls back to
    /// the documented defaults.
    std::vector<block_mining_spec> block_minings{};
    std::vector<tool_mining_spec> tools{};
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
    std::uint32_t light_shapes{0};
    std::uint32_t tools{0};
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

/// One occlusion box of a light face shape, in block units.
struct light_box_view {
    std::array<double, 6> bounds{};
};

/// The box counts of one light shape's six faces.
struct light_shape_view {
    std::array<std::uint16_t, light_shape_faces> face_boxes{};
};

/// Mining behaviour of one block as stored in the container.
struct block_mining_view {
    /// Destroy time; `-1` means unbreakable.
    float hardness{-1.0F};
    /// Mining speed of shears on the block.
    float shears_speed{1.0F};
    /// Mining speed of a sword on the block.
    float sword_speed{1.0F};
    /// `block_mining_*` flag bits.
    std::uint8_t flags{0};
    /// Tool families that mine the block efficiently.
    std::uint8_t tool_mask{0};
    /// Lowest tool-material harvest level that yields drops.
    std::uint8_t needed_tier{0};
    /// Tool families that yield drops without a tier threshold.
    std::uint8_t direct_correct{0};

    [[nodiscard]] auto requires_correct_tool_for_drops() const noexcept
        -> bool {
        return (flags & block_mining_requires_correct_tool) != 0;
    }
    [[nodiscard]] auto is_air() const noexcept -> bool {
        return (flags & block_mining_air) != 0;
    }
    [[nodiscard]] auto is_fluid() const noexcept -> bool {
        return (flags & block_mining_fluid) != 0;
    }
    [[nodiscard]] auto unbreakable() const noexcept -> bool {
        return hardness < 0.0F;
    }
};

/// One tool item as stored in the container.
struct tool_mining_view {
    std::string_view name{};
    tool_type type{tool_type::none};
    tool_material material{tool_material::none};
    std::uint8_t harvest_level{harvest_level_none};
    float tier_speed{1.0F};
    float default_mining_speed{1.0F};
    std::uint16_t damage_per_block{0};
    std::uint16_t max_stack_size{0};
    std::uint32_t max_damage{0};
    bool can_destroy_blocks_in_creative{true};
};

/// Light behaviour of one state as stored in the container.
struct block_light_view {
    std::uint8_t emission{0};
    std::uint8_t dampening{15};
    bool can_occlude{true};
    bool use_shape_for_light_occlusion{false};
    /// Index into the light shape table, or `no_light_shape`.
    std::uint32_t shape{no_light_shape};
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

    /// Whether the container carries per-state light records. A container
    /// imported without the block light report does not, and the runtime falls
    /// back to its documented defaults for every state.
    [[nodiscard]] auto has_light() const noexcept -> bool {
        return have_light_;
    }
    /// Light record of one state, in the container's dense state order.
    [[nodiscard]] auto light_at(std::uint32_t state) const -> block_light_view;
    [[nodiscard]] auto light_shape_count() const noexcept -> std::uint32_t {
        return light_shapes_;
    }
    [[nodiscard]] auto light_shape_at(std::uint32_t shape) const
        -> light_shape_view;
    /// One box of one face of a light shape; the caller has bounds-checked the
    /// shape and index through `light_shape_at`.
    [[nodiscard]] auto light_box(std::uint32_t shape, std::uint8_t face,
                                 std::uint16_t index) const -> light_box_view;

    /// Whether the container carries mining data. A container imported without
    /// the block mining report does not, and the runtime treats every block as
    /// unbreakable rather than mining it with a guessed speed.
    [[nodiscard]] auto has_mining() const noexcept -> bool {
        return have_mining_;
    }
    /// Mining record of one dense block, in the container's block order.
    [[nodiscard]] auto block_mining_at(std::uint16_t block) const
        -> block_mining_view;
    /// Tool items in the container, sorted by name. The table is empty when
    /// the container carries no mining data.
    [[nodiscard]] auto tool_count() const noexcept -> std::uint32_t {
        return info_.tools;
    }
    [[nodiscard]] auto tool_at(std::uint32_t index) const -> tool_mining_view;
    /// Binary search over the name-sorted tool records.
    [[nodiscard]] auto find_tool(std::string_view name) const
        -> std::optional<std::uint32_t>;

  private:
    struct section {
        std::uint32_t offset{0};
        std::uint32_t size{0};
    };

    [[nodiscard]] auto string_at(std::uint32_t index) const -> std::string_view;

    /// The encoded container, borrowed. The caller owns the bytes and must
    /// keep them alive for as long as this view is used: every accessor reads
    /// through this span.
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
    section light_states_{};
    section light_shapes_section_{};
    section mining_{};
    section tools_section_{};
    section section_table_{};
    vanilla_table_info info_{};
    bool have_light_{false};
    bool have_mining_{false};
    std::uint32_t light_shapes_{0};
    std::uint32_t light_boxes_{0};

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
    /// The `block_light/light.json` report, empty when the reports directory
    /// carries none. A container imported without it has no light sections.
    std::string block_light_json{};
    /// The `block_mining/mining.json` report, empty when the reports directory
    /// carries none. A container imported without it has no mining sections.
    std::string block_mining_json{};
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
    std::uint32_t light_shapes{0};
    std::uint32_t tools{0};
};

/// Reads `blocks.json`, `biome_parameters/minecraft/*.json`,
/// `registries.json`, `minecraft/components/item/*.json` and, when present,
/// `block_light/light.json` and `block_mining/mining.json` from
/// `options.reports`, writes `tables.rvtb` and
/// `MANIFEST.json` into `options.out`, and returns their hashes.
[[nodiscard]] auto import_vanilla_reports(const vanilla_import_options &options)
    -> result<vanilla_import_result>;

} // namespace refrain::data
