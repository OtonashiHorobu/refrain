// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/worldgen/feature/block_predicate.hpp>

#include <refrain/data/json.hpp>

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace refrain::worldgen::feature {

namespace {

auto type_of(const data::json::value &input) -> result<std::string_view> {
    const auto *type = input.find("type");
    if (type == nullptr) {
        return std::unexpected{
            make_error(error_code::parse_error, "block predicate has no type")};
    }
    const auto text = type->as_string();
    if (!text) {
        return std::unexpected{make_error(
            error_code::parse_error, "block predicate type is not a string")};
    }
    return *text;
}

auto names(const data::json::value &input) -> result<std::vector<std::string>> {
    if (const auto single = input.as_string(); single.has_value()) {
        return std::vector<std::string>{std::string{*single}};
    }
    if (!input.is_array()) {
        return std::unexpected{
            make_error(error_code::parse_error, "block list is not an array")};
    }
    std::vector<std::string> out;
    for (const auto &entry : *input.as_array()) {
        const auto name = entry.as_string();
        if (!name) {
            return std::unexpected{make_error(error_code::parse_error,
                                              "block name is not a string")};
        }
        out.emplace_back(*name);
    }
    return out;
}

class matching_blocks final : public block_predicate {
  public:
    explicit matching_blocks(std::vector<std::string> blocks)
        : blocks_{std::move(blocks)} {}
    [[nodiscard]] auto test(const chunk_region &region,
                            const block_state_table &states,
                            const block_pos &pos) const -> bool override {
        const auto name = states.name(region.state_at(pos.x, pos.y, pos.z));
        return std::find(blocks_.begin(), blocks_.end(), name) != blocks_.end();
    }

  private:
    std::vector<std::string> blocks_;
};

class matching_tag final : public block_predicate {
  public:
    explicit matching_tag(std::string tag) : tag_{std::move(tag)} {}
    [[nodiscard]] auto test(const chunk_region &region,
                            const block_state_table &states,
                            const block_pos &pos) const -> bool override {
        return states.in_tag(region.state_at(pos.x, pos.y, pos.z), tag_);
    }

  private:
    std::string tag_;
};

class not_predicate final : public block_predicate {
  public:
    explicit not_predicate(std::unique_ptr<block_predicate> inner)
        : inner_{std::move(inner)} {}
    [[nodiscard]] auto test(const chunk_region &region,
                            const block_state_table &states,
                            const block_pos &pos) const -> bool override {
        return !inner_->test(region, states, pos);
    }

  private:
    std::unique_ptr<block_predicate> inner_;
};

class any_of final : public block_predicate {
  public:
    explicit any_of(std::vector<std::unique_ptr<block_predicate>> rules)
        : rules_{std::move(rules)} {}
    [[nodiscard]] auto test(const chunk_region &region,
                            const block_state_table &states,
                            const block_pos &pos) const -> bool override {
        return std::any_of(rules_.begin(), rules_.end(), [&](const auto &rule) {
            return rule->test(region, states, pos);
        });
    }

  private:
    std::vector<std::unique_ptr<block_predicate>> rules_;
};

class solid_predicate final : public block_predicate {
  public:
    [[nodiscard]] auto test(const chunk_region &region,
                            const block_state_table &states,
                            const block_pos &pos) const -> bool override {
        return states.traits(region.state_at(pos.x, pos.y, pos.z))
            .blocks_motion;
    }
};

class has_sturdy_face final : public block_predicate {
  public:
    has_sturdy_face(std::string_view direction, const block_pos &offset)
        : direction_{direction}, offset_{offset} {}
    [[nodiscard]] auto test(const chunk_region &region,
                            const block_state_table &states,
                            const block_pos &pos) const -> bool override {
        const block_pos target{.x = pos.x + offset_.x,
                               .y = pos.y + offset_.y,
                               .z = pos.z + offset_.z};
        return states.traits(region.state_at(target.x, target.y, target.z))
            .blocks_motion;
    }

  private:
    std::string direction_;
    block_pos offset_{};
};

class matching_fluids final : public block_predicate {
  public:
    matching_fluids(bool water, bool lava) : water_{water}, lava_{lava} {}
    [[nodiscard]] auto test(const chunk_region &region,
                            const block_state_table &states,
                            const block_pos &pos) const -> bool override {
        const auto traits = states.traits(region.state_at(pos.x, pos.y, pos.z));
        return (water_ && traits.water) ||
               (lava_ && traits.fluid && !traits.water);
    }

  private:
    bool water_{false};
    bool lava_{false};
};

class all_of final : public block_predicate {
  public:
    explicit all_of(std::vector<std::unique_ptr<block_predicate>> rules)
        : rules_{std::move(rules)} {}
    [[nodiscard]] auto test(const chunk_region &region,
                            const block_state_table &states,
                            const block_pos &pos) const -> bool override {
        return std::all_of(rules_.begin(), rules_.end(), [&](const auto &rule) {
            return rule->test(region, states, pos);
        });
    }

  private:
    std::vector<std::unique_ptr<block_predicate>> rules_;
};

} // namespace

auto decode_block_predicate(const data::json::value &document,
                            const block_state_table &states)
    -> result<std::unique_ptr<block_predicate>> {
    const auto type = type_of(document);
    if (!type) {
        return std::unexpected{type.error()};
    }
    if (*type == "minecraft:matching_blocks" ||
        *type == "minecraft:matching_block_tag") {
        const bool tag = *type == "minecraft:matching_block_tag";
        const auto *field = document.find(tag ? "tag" : "blocks");
        if (field == nullptr) {
            return std::unexpected{make_error(error_code::parse_error,
                                              "block predicate has no list")};
        }
        if (tag) {
            const auto name = field->as_string();
            if (!name) {
                return std::unexpected{
                    make_error(error_code::parse_error, "tag is not a string")};
            }
            return std::unique_ptr<block_predicate>{
                std::make_unique<matching_tag>(std::string{*name})};
        }
        auto list = names(*field);
        if (!list) {
            return std::unexpected{list.error()};
        }
        return std::unique_ptr<block_predicate>{
            std::make_unique<matching_blocks>(std::move(*list))};
    }
    if (*type == "minecraft:has_sturdy_face") {
        const auto *direction = document.find("direction");
        const auto *offset = document.find("offset");
        if (direction == nullptr || offset == nullptr || !offset->is_array()) {
            return std::unexpected{make_error(error_code::parse_error,
                                              "has_sturdy_face is incomplete")};
        }
        const auto direction_text = direction->as_string();
        if (!direction_text) {
            return std::unexpected{direction_text.error()};
        }
        const auto values = offset->as_array();
        if (!values || values->size() != 3) {
            return std::unexpected{make_error(
                error_code::parse_error, "has_sturdy_face offset is invalid")};
        }
        const auto x = (*values)[0].as_int64();
        const auto y = (*values)[1].as_int64();
        const auto z = (*values)[2].as_int64();
        if (!x || !y || !z) {
            return std::unexpected{make_error(
                error_code::parse_error, "has_sturdy_face offset is invalid")};
        }
        return std::unique_ptr<block_predicate>{
            std::make_unique<has_sturdy_face>(
                *direction_text, block_pos{.x = static_cast<jint>(*x),
                                           .y = static_cast<jint>(*y),
                                           .z = static_cast<jint>(*z)})};
    }
    if (*type == "minecraft:solid") {
        return std::unique_ptr<block_predicate>{
            std::make_unique<solid_predicate>()};
    }
    if (*type == "minecraft:matching_fluids") {
        const auto *fluids = document.find("fluids");
        if (fluids == nullptr) {
            return std::unexpected{make_error(error_code::parse_error,
                                              "matching_fluids has no fluids")};
        }
        auto list = names(*fluids);
        if (!list) {
            return std::unexpected{list.error()};
        }
        bool water = false;
        bool lava = false;
        for (const auto &name : *list) {
            if (name == "minecraft:water") {
                water = true;
            } else if (name == "minecraft:lava") {
                lava = true;
            }
        }
        return std::unique_ptr<block_predicate>{
            std::make_unique<matching_fluids>(water, lava)};
    }
    if (*type == "minecraft:not") {
        const auto *predicate = document.find("predicate");
        if (predicate == nullptr) {
            return std::unexpected{make_error(
                error_code::parse_error, "not predicate has no predicate")};
        }
        auto inner = decode_block_predicate(*predicate, states);
        if (!inner) {
            return std::unexpected{inner.error()};
        }
        return std::unique_ptr<block_predicate>{
            std::make_unique<not_predicate>(std::move(*inner))};
    }
    if (*type == "minecraft:any_of") {
        const auto *rules = document.find("predicates");
        if (rules == nullptr || !rules->is_array()) {
            return std::unexpected{make_error(error_code::parse_error,
                                              "any_of has no predicate list")};
        }
        std::vector<std::unique_ptr<block_predicate>> decoded;
        for (const auto &entry : *rules->as_array()) {
            auto predicate = decode_block_predicate(entry, states);
            if (!predicate) {
                return std::unexpected{predicate.error()};
            }
            decoded.push_back(std::move(*predicate));
        }
        return std::unique_ptr<block_predicate>{
            std::make_unique<any_of>(std::move(decoded))};
    }
    if (*type == "minecraft:all_of") {
        const auto *rules = document.find("predicates");
        if (rules == nullptr) {
            rules = document.find("rules");
        }
        if (rules == nullptr || !rules->is_array()) {
            return std::unexpected{make_error(error_code::parse_error,
                                              "all_of has no predicate list")};
        }
        std::vector<std::unique_ptr<block_predicate>> decoded;
        for (const auto &entry : *rules->as_array()) {
            auto predicate = decode_block_predicate(entry, states);
            if (!predicate) {
                return std::unexpected{predicate.error()};
            }
            decoded.push_back(std::move(*predicate));
        }
        return std::unique_ptr<block_predicate>{
            std::make_unique<all_of>(std::move(decoded))};
    }
    return std::unexpected{
        make_error(error_code::unsupported, "unsupported block predicate")};
}

} // namespace refrain::worldgen::feature
