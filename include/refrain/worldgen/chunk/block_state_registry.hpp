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
class block_state_registry final : public block_state_table {
  public:
    block_state_registry();

    /// Interns a state key; returns the existing id when the key is known.
    [[nodiscard]] auto intern(std::string_view key, const state_traits &traits,
                              std::uint16_t block = 0,
                              std::uint16_t vanilla_id = no_block_state)
        -> result<block_state_id>;

    [[nodiscard]] auto find(std::string_view key) const
        -> std::optional<block_state_id>;
    [[nodiscard]] auto size() const noexcept -> std::size_t {
        return keys_.size();
    }
    [[nodiscard]] auto block(block_state_id id) const -> std::uint16_t;
    [[nodiscard]] auto vanilla_id(block_state_id id) const -> std::uint16_t;

    void add_tag(std::string_view tag, std::span<const block_state_id> states);

    // block_state_table
    [[nodiscard]] auto air() const -> block_state_id override;
    [[nodiscard]] auto traits(block_state_id id) const -> state_traits override;
    [[nodiscard]] auto name(block_state_id id) const
        -> std::string_view override;
    [[nodiscard]] auto in_tag(block_state_id id, std::string_view tag) const
        -> bool override;

  private:
    std::deque<std::string> keys_{};
    std::vector<state_traits> traits_{};
    std::vector<std::uint16_t> blocks_{};
    std::vector<std::uint16_t> vanilla_ids_{};
    std::unordered_map<std::string_view, block_state_id> index_{};
    std::map<std::string, std::vector<block_state_id>, std::less<>> tags_{};
    std::map<std::string, std::set<std::string, std::less<>>, std::less<>>
        tag_names_{};
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

} // namespace refrain::worldgen
