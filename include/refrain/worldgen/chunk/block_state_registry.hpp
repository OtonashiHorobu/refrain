// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// The concrete block-state table used by generation. It owns one name per
// state, the traits the heightmaps and features test, and the block tags the
// data pack defines. Names and tags are fixed after loading; queries are
// allocation-free.

#pragma once

#include <refrain/core/error.hpp>
#include <refrain/worldgen/chunk/block_state.hpp>

#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace refrain::data {
class vanilla_tables;
} // namespace refrain::data

namespace refrain::worldgen {

/// Raw `tags/block` documents before block names are resolved to state ids.
class block_tag_set {
  public:
    /// Adds one tag; `entries` may name blocks or reference other tags with a
    /// leading `#`.
    void add(std::string_view tag, std::vector<std::string> entries);

    [[nodiscard]] auto has(std::string_view tag) const -> bool;

    /// Resolves a tag to its block names, following nested tags. Returns
    /// `invalid_argument` for a missing tag or a reference cycle.
    [[nodiscard]] auto resolve(std::string_view tag) const
        -> result<std::vector<std::string>>;

    [[nodiscard]] auto entries() const -> const
        std::map<std::string, std::vector<std::string>, std::less<>> & {
        return entries_;
    }

  private:
    auto resolve_into(std::string_view tag, std::vector<std::string> &out,
                      std::vector<std::string> &visiting) const -> status;

    std::map<std::string, std::vector<std::string>, std::less<>> entries_{};
    mutable std::map<std::string, std::vector<std::string>, std::less<>>
        resolved_{};
};

/// Reads `<data_root>/<namespace>/tags/block/**/*.json` for every namespace
/// directory present. A missing `tags/block` directory yields an empty set.
[[nodiscard]] auto load_block_tags(std::string_view data_root)
    -> result<block_tag_set>;

/// Concrete `block_state_table`. State ids are assigned in insertion order;
/// `minecraft:air` is always id 0.
///
/// A registry built from the content's block-state table is complete and
/// frozen before generation starts: the ids then come from the table's
/// key-sorted state list, so the same state key always gets the same id no
/// matter which worker reaches it first. Freezing is what lets one registry
/// serve several generation workers: a frozen table only reads, so concurrent
/// lookups need no lock, and an unknown key fails instead of taking a first-use
/// id that would depend on thread interleaving.
class block_state_registry final : public block_state_table {
  public:
    block_state_registry();

    /// Interns a state key; returns the existing id when the key is known.
    /// A frozen table rejects a key it does not hold.
    [[nodiscard]] auto intern(std::string_view key, const state_traits &traits,
                              std::uint16_t block = 0,
                              std::uint16_t vanilla_id = no_block_state)
        -> result<block_state_id>;

    /// Interns a key that only generation may resolve, not `find`: the id is
    /// assigned now, before the table is frozen, but a lookup by key stays a
    /// miss. A shared table uses it to hold every state the content can derive
    /// while keeping the lookup behaviour the per-generator registry has,
    /// where `find` only sees keys a data pack interned.
    [[nodiscard]] auto intern_shadow(std::string_view key,
                                     const state_traits &traits,
                                     std::uint16_t block = 0,
                                     std::uint16_t vanilla_id = no_block_state)
        -> result<block_state_id>;

    /// Freezes the table: no key can be added afterwards. Call it after the
    /// complete state universe is interned and before any concurrent reader
    /// starts.
    void freeze() noexcept { frozen_ = true; }
    [[nodiscard]] auto frozen() const noexcept -> bool { return frozen_; }

    /// Looks a state up by its canonical key. A bare block name resolves to
    /// the block's default state, which is how the datapack spells a state
    /// when it names no properties.
    [[nodiscard]] auto find(std::string_view key) const
        -> std::optional<block_state_id>;
    [[nodiscard]] auto size() const noexcept -> std::size_t {
        return keys_.size();
    }
    [[nodiscard]] auto block(block_state_id id) const -> std::uint16_t;
    [[nodiscard]] auto vanilla_id(block_state_id id) const -> std::uint16_t;

    /// Records the default state of `block`. `find` resolves a bare block
    /// name to it, and the registry builder fills it from the block table.
    void add_default_state(std::string_view block, block_state_id state);

    /// Replaces the default-property table. A datapack state key that names a
    /// block without properties can then answer the state-dependent questions
    /// the reference asks its default state.
    void set_block_defaults(block_default_map defaults);

    void add_tag(std::string_view tag, std::span<const block_state_id> states);

    /// Tag members in the order the tag document listed them, without
    /// duplicates. Empty when the tag is unknown.
    [[nodiscard]] auto tag_states(std::string_view tag) const
        -> std::span<const block_state_id>;

    /// Traits for a state key that is not interned yet. When the registry
    /// carries the heightmap motion tags, membership is decided by the block
    /// name the tags list, matching how reference worlds classify states.
    /// Without those tags the name alone decides: air and fluids never block,
    /// leaves are excluded from the leaf-less variant.
    [[nodiscard]] auto infer_traits(std::string_view key) const -> state_traits;

    // block_state_table
    [[nodiscard]] auto air() const -> block_state_id override;
    [[nodiscard]] auto traits(block_state_id id) const -> state_traits override;
    [[nodiscard]] auto name(block_state_id id) const
        -> std::string_view override;
    [[nodiscard]] auto in_tag(block_state_id id, std::string_view tag) const
        -> bool override;
    [[nodiscard]] auto default_properties(block_state_id id) const
        -> std::span<const block_default_property> override;

    /// Default properties of one block by block name, empty when the table
    /// carries no default-state data for it.
    [[nodiscard]] auto
    default_properties_for_block(std::string_view block) const
        -> std::span<const block_default_property>;

  private:
    std::deque<std::string> keys_{};
    std::vector<state_traits> traits_{};
    std::vector<std::uint16_t> blocks_{};
    std::vector<std::uint16_t> vanilla_ids_{};
    std::unordered_map<std::string_view, block_state_id> index_{};
    /// Keys interned by `intern_shadow`: resolvable by `intern`, invisible to
    /// `find`.
    std::unordered_map<std::string_view, block_state_id> shadow_index_{};
    std::map<std::string, block_state_id, std::less<>> defaults_{};
    block_default_map default_properties_{};
    std::map<std::string, std::vector<block_state_id>, std::less<>> tags_{};
    std::map<std::string, std::vector<block_state_id>, std::less<>>
        ordered_tags_{};
    std::map<std::string, std::set<std::string, std::less<>>, std::less<>>
        tag_names_{};
    bool frozen_{false};
};

/// Registers every tag target on `states`, interning target names that are not
/// present with traits inferred from the name. Data readers that build a
/// registry from generation output rather than the vanilla block table use it
/// so predicates can query block tags.
[[nodiscard]] auto apply_block_tags(block_state_registry &states,
                                    const block_tag_set &tags) -> status;

/// Builds a registry from an RVTB block table and the data pack's block tags.
/// The tags supply the two motion-blocking traits; `waterlogged=true` and the
/// water/lava block names supply the fluid traits.
[[nodiscard]] auto
build_block_state_registry(const data::vanilla_tables &tables,
                           const block_tag_set &tags)
    -> result<block_state_registry>;

/// Builds the complete registry a scheduler's shared generation state uses.
/// Every state of the content table is interned, in the table's key order (the
/// block table sorts blocks by name and the states of a block by key), so a
/// state id is fixed before any worker starts and never depends on which
/// worker first asks for a key. The default state of a block is interned under
/// the bare block name as well as under its canonical spelling: generation
/// folds a default-property spelling to the bare name, and both spellings must
/// resolve without appending a key after the table is frozen.
[[nodiscard]] auto
build_shared_block_state_registry(const data::vanilla_tables &tables,
                                  const block_tag_set &tags)
    -> result<block_state_registry>;

} // namespace refrain::worldgen
