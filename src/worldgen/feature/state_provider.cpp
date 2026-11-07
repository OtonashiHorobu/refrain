// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horashi

#include <refrain/worldgen/feature/state_provider.hpp>

#include <refrain/data/json.hpp>
#include <refrain/worldgen/chunk/block_state_codec.hpp>
#include <refrain/worldgen/chunk/block_state_registry.hpp>
#include <refrain/worldgen/feature/placement.hpp>

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace refrain::worldgen::feature {

namespace {

auto resolve_state(block_state_registry &states, std::string_view key)
    -> result<block_state_id> {
    if (const auto found = states.find(key); found.has_value()) {
        return *found;
    }
    const auto block = state_block_name(key);
    const bool air = is_air_block_name(block);
    const bool water = block == "minecraft:water" ||
                       key.find("waterlogged=true") != std::string_view::npos;
    const bool fluid = water || block == "minecraft:lava" ||
                       block == "minecraft:bubble_column";
    return states.intern(
        key, state_traits{.air = air,
                          .fluid = fluid,
                          .water = water,
                          .blocks_motion = !air && !fluid,
                          .blocks_motion_no_leaves = !air && !fluid});
}

class simple_provider final : public state_provider {
  public:
    explicit simple_provider(block_state_id state) : state_{state} {}
    [[nodiscard]] auto get(const placement_context &, worldgen_random &,
                           const block_pos &) const -> block_state_id override {
        return state_;
    }

  private:
    block_state_id state_{no_block_state};
};

class weighted_provider final : public state_provider {
  public:
    weighted_provider(std::vector<std::pair<jint, block_state_id>> entries,
                      jint total)
        : entries_{std::move(entries)}, total_{total} {}
    [[nodiscard]] auto get(const placement_context &, worldgen_random &random,
                           const block_pos &) const -> block_state_id override {
        jint selection = random.next_int(total_);
        for (const auto &[weight, state] : entries_) {
            selection -= weight;
            if (selection < 0) {
                return state;
            }
        }
        return entries_.back().second;
    }

  private:
    std::vector<std::pair<jint, block_state_id>> entries_;
    jint total_{0};
};

} // namespace

auto simple_state(block_state_id state) -> std::unique_ptr<state_provider> {
    return std::make_unique<simple_provider>(state);
}

auto decode_state_provider(const data::json::value &document,
                           block_state_registry &states)
    -> result<std::unique_ptr<state_provider>> {
    if (!document.is_object()) {
        const auto key = decode_state_key(document);
        if (!key) {
            return std::unexpected{key.error()};
        }
        auto state = resolve_state(states, *key);
        if (!state) {
            return std::unexpected{state.error()};
        }
        return std::unique_ptr<state_provider>{
            std::make_unique<simple_provider>(*state)};
    }
    const auto *type_value = document.find("type");
    if (type_value == nullptr) {
        const auto key = decode_state_key(document);
        if (!key) {
            return std::unexpected{key.error()};
        }
        auto state = resolve_state(states, *key);
        if (!state) {
            return std::unexpected{state.error()};
        }
        return std::unique_ptr<state_provider>{
            std::make_unique<simple_provider>(*state)};
    }
    const auto type = type_value->as_string();
    if (!type) {
        return std::unexpected{make_error(
            error_code::parse_error, "state provider type is not a string")};
    }
    if (*type == "minecraft:simple" ||
        *type == "minecraft:simple_state_provider") {
        const auto *state_value = document.find("state");
        if (state_value == nullptr) {
            return std::unexpected{make_error(error_code::parse_error,
                                              "simple provider has no state")};
        }
        const auto key = decode_state_key(*state_value);
        if (!key) {
            return std::unexpected{key.error()};
        }
        auto state = resolve_state(states, *key);
        if (!state) {
            return std::unexpected{state.error()};
        }
        return std::unique_ptr<state_provider>{
            std::make_unique<simple_provider>(*state)};
    }
    if (*type == "minecraft:weighted") {
        const auto *entries = document.find("entries");
        if (entries == nullptr || !entries->is_array()) {
            return std::unexpected{make_error(
                error_code::parse_error, "weighted provider has no entries")};
        }
        std::vector<std::pair<jint, block_state_id>> decoded;
        jint total = 0;
        for (const auto &entry : *entries->as_array()) {
            const auto *data = entry.find("data");
            const auto *weight = entry.find("weight");
            if (data == nullptr || weight == nullptr) {
                return std::unexpected{make_error(
                    error_code::parse_error, "weighted entry is incomplete")};
            }
            const auto weight_value = weight->as_int64();
            const auto key = decode_state_key(*data);
            if (!weight_value || !key) {
                return std::unexpected{make_error(error_code::parse_error,
                                                  "weighted entry is invalid")};
            }
            auto state = resolve_state(states, *key);
            if (!state) {
                return std::unexpected{state.error()};
            }
            total += static_cast<jint>(*weight_value);
            decoded.emplace_back(static_cast<jint>(*weight_value), *state);
        }
        return std::unique_ptr<state_provider>{
            std::make_unique<weighted_provider>(std::move(decoded), total)};
    }
    return std::unexpected{
        make_error(error_code::unsupported, "unsupported state provider")};
}

} // namespace refrain::worldgen::feature
