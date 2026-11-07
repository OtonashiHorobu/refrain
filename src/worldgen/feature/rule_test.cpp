// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horashi

#include <refrain/worldgen/feature/rule_test.hpp>

#include <refrain/data/json.hpp>

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace refrain::worldgen::feature {

namespace {

auto predicate_type(const data::json::value &input)
    -> result<std::string_view> {
    const auto *type = input.find("predicate_type");
    if (type == nullptr) {
        return std::unexpected{make_error(error_code::parse_error,
                                          "rule test has no predicate_type")};
    }
    const auto text = type->as_string();
    if (!text) {
        return std::unexpected{make_error(error_code::parse_error,
                                          "rule test type is not a string")};
    }
    return *text;
}

class always_true final : public rule_test {
  public:
    [[nodiscard]] auto test(block_state_id, const block_pos &,
                            worldgen_random &) const -> bool override {
        return true;
    }
};

class tag_match final : public rule_test {
  public:
    tag_match(std::string tag, const block_state_table &states)
        : tag_{std::move(tag)}, states_{&states} {}
    [[nodiscard]] auto test(block_state_id state, const block_pos &,
                            worldgen_random &) const -> bool override {
        return states_->in_tag(state, tag_);
    }

  private:
    std::string tag_;
    const block_state_table *states_{nullptr};
};

class block_match final : public rule_test {
  public:
    block_match(std::string block, const block_state_table &states)
        : block_{std::move(block)}, states_{&states} {}
    [[nodiscard]] auto test(block_state_id state, const block_pos &,
                            worldgen_random &) const -> bool override {
        const auto name = states_->name(state);
        return name == block_ || (name.size() > block_.size() &&
                                  name.substr(0, block_.size()) == block_ &&
                                  name[block_.size()] == '[');
    }

  private:
    std::string block_;
    const block_state_table *states_{nullptr};
};

class blockstate_match final : public rule_test {
  public:
    blockstate_match(std::string state_name, const block_state_table &states)
        : state_{std::move(state_name)}, states_{&states} {}
    [[nodiscard]] auto test(block_state_id state, const block_pos &,
                            worldgen_random &) const -> bool override {
        return states_->name(state) == state_;
    }

  private:
    std::string state_;
    const block_state_table *states_{nullptr};
};

class height_match final : public rule_test {
  public:
    height_match(jint min, jint max) : min_{min}, max_{max} {}
    [[nodiscard]] auto test(block_state_id, const block_pos &pos,
                            worldgen_random &) const -> bool override {
        return pos.y >= min_ && pos.y <= max_;
    }

  private:
    jint min_{0};
    jint max_{0};
};

class all_of final : public rule_test {
  public:
    explicit all_of(std::vector<std::unique_ptr<rule_test>> rules)
        : rules_{std::move(rules)} {}
    [[nodiscard]] auto test(block_state_id state, const block_pos &pos,
                            worldgen_random &random) const -> bool override {
        return std::all_of(rules_.begin(), rules_.end(), [&](const auto &rule) {
            return rule->test(state, pos, random);
        });
    }

  private:
    std::vector<std::unique_ptr<rule_test>> rules_;
};

class any_of final : public rule_test {
  public:
    explicit any_of(std::vector<std::unique_ptr<rule_test>> rules)
        : rules_{std::move(rules)} {}
    [[nodiscard]] auto test(block_state_id state, const block_pos &pos,
                            worldgen_random &random) const -> bool override {
        return std::any_of(rules_.begin(), rules_.end(), [&](const auto &rule) {
            return rule->test(state, pos, random);
        });
    }

  private:
    std::vector<std::unique_ptr<rule_test>> rules_;
};

class not_rule final : public rule_test {
  public:
    explicit not_rule(std::unique_ptr<rule_test> rule)
        : rule_{std::move(rule)} {}
    [[nodiscard]] auto test(block_state_id state, const block_pos &pos,
                            worldgen_random &random) const -> bool override {
        return !rule_->test(state, pos, random);
    }

  private:
    std::unique_ptr<rule_test> rule_;
};

auto state_name(const data::json::value &input) -> result<std::string> {
    if (const auto text = input.as_string(); text.has_value()) {
        return std::string{*text};
    }
    if (input.is_object()) {
        const auto *id = input.find("id");
        if (id == nullptr) {
            id = input.find("Name");
        }
        if (id != nullptr) {
            if (const auto text = id->as_string(); text.has_value()) {
                return std::string{*text};
            }
        }
    }
    return std::unexpected{
        make_error(error_code::parse_error, "state is not a string")};
}

} // namespace

auto decode_rule_test(const data::json::value &document,
                      const block_state_table &states)
    -> result<std::unique_ptr<rule_test>> {
    const auto type = predicate_type(document);
    if (!type) {
        return std::unexpected{type.error()};
    }
    if (*type == "minecraft:always_true") {
        return std::unique_ptr<rule_test>{std::make_unique<always_true>()};
    }
    if (*type == "minecraft:tag_match") {
        const auto *tag = document.find("tag");
        if (tag == nullptr) {
            return std::unexpected{
                make_error(error_code::parse_error, "tag_match has no tag")};
        }
        const auto name = tag->as_string();
        if (!name) {
            return std::unexpected{
                make_error(error_code::parse_error, "tag is not a string")};
        }
        return std::unique_ptr<rule_test>{
            std::make_unique<tag_match>(std::string{*name}, states)};
    }
    if (*type == "minecraft:block_match" ||
        *type == "minecraft:blockstate_match") {
        const auto *block = document.find("block");
        if (block == nullptr) {
            return std::unexpected{make_error(error_code::parse_error,
                                              "block match has no block")};
        }
        auto name = state_name(*block);
        if (!name) {
            return std::unexpected{name.error()};
        }
        if (*type == "minecraft:block_match") {
            return std::unique_ptr<rule_test>{
                std::make_unique<block_match>(std::move(*name), states)};
        }
        return std::unique_ptr<rule_test>{
            std::make_unique<blockstate_match>(std::move(*name), states)};
    }
    if (*type == "minecraft:height_match") {
        const auto *min = document.find("min_inclusive");
        const auto *max = document.find("max_inclusive");
        if (min == nullptr || max == nullptr) {
            return std::unexpected{make_error(error_code::parse_error,
                                              "height_match has no range")};
        }
        const auto min_value = min->as_int64();
        const auto max_value = max->as_int64();
        if (!min_value || !max_value) {
            return std::unexpected{make_error(error_code::parse_error,
                                              "height_match range is invalid")};
        }
        return std::unique_ptr<rule_test>{std::make_unique<height_match>(
            static_cast<jint>(*min_value), static_cast<jint>(*max_value))};
    }
    if (*type == "minecraft:all_of" || *type == "minecraft:any_of") {
        const auto *rules = document.find("rules");
        if (rules == nullptr || !rules->is_array()) {
            return std::unexpected{
                make_error(error_code::parse_error, "rule list is missing")};
        }
        std::vector<std::unique_ptr<rule_test>> decoded;
        for (const auto &entry : *rules->as_array()) {
            auto rule = decode_rule_test(entry, states);
            if (!rule) {
                return std::unexpected{rule.error()};
            }
            decoded.push_back(std::move(*rule));
        }
        if (*type == "minecraft:all_of") {
            return std::unique_ptr<rule_test>{
                std::make_unique<all_of>(std::move(decoded))};
        }
        return std::unique_ptr<rule_test>{
            std::make_unique<any_of>(std::move(decoded))};
    }
    if (*type == "minecraft:not") {
        const auto *rule = document.find("rule");
        if (rule == nullptr) {
            return std::unexpected{
                make_error(error_code::parse_error, "not has no rule")};
        }
        auto decoded = decode_rule_test(*rule, states);
        if (!decoded) {
            return std::unexpected{decoded.error()};
        }
        return std::unique_ptr<rule_test>{
            std::make_unique<not_rule>(std::move(*decoded))};
    }
    return std::unexpected{
        make_error(error_code::unsupported, "unsupported rule test")};
}

} // namespace refrain::worldgen::feature
