// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/mod/install_policy.hpp>

#include <refrain/core/config.hpp>
#include <refrain/core/io.hpp>
#include <refrain/data/signature.hpp>

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace refrain::mod {
namespace {

namespace config = core::config;
namespace data = refrain::data;

[[nodiscard]] auto policy_error(std::string_view path, std::string message)
    -> error {
    return make_error(error_code::parse_error,
                      fmt::format("mods policy: '{}': {}", path, message));
}

[[nodiscard]] auto
reject_unknown_keys(const config::document &document,
                    std::span<const std::string_view> path_component,
                    std::span<const std::string_view> allowed,
                    std::string_view table_name, std::string_view file)
    -> status {
    auto keys{document.keys_at(path_component)};
    if (!keys) {
        return std::unexpected{policy_error(
            file, fmt::format("[{}]: {}", table_name, keys.error().message))};
    }
    for (const auto &key : *keys) {
        const auto known{std::find(allowed.begin(), allowed.end(), key) !=
                         allowed.end()};
        if (!known) {
            return std::unexpected{
                policy_error(file, fmt::format("[{}] has unknown key '{}'",
                                               table_name, key))};
        }
    }
    return status_ok();
}

[[nodiscard]] auto policy_bool(const config::document &document,
                               std::span<const std::string_view> path,
                               std::string_view name, std::string_view file)
    -> result<bool> {
    if (!document.contains_at(path)) {
        return false;
    }
    auto value{document.get_bool_at(path)};
    if (!value) {
        return std::unexpected{
            policy_error(file, fmt::format("'{}' must be a boolean", name))};
    }
    return *value;
}

[[nodiscard]] auto policy_string(const config::document &document,
                                 std::span<const std::string_view> path,
                                 std::string_view name, std::string_view file)
    -> result<std::string> {
    if (!document.contains_at(path)) {
        return std::string{};
    }
    auto value{document.get_string_at(path)};
    if (!value) {
        return std::unexpected{
            policy_error(file, fmt::format("'{}' must be a string", name))};
    }
    return std::move(*value);
}

[[nodiscard]] auto read_signature_table(const config::document &document,
                                        std::string_view file,
                                        install_policy_config &policy)
    -> status {
    const std::array<std::string_view, 1> path{"signature"};
    const auto kind{document.kind_at(path)};
    if (kind == config::value_kind::missing) {
        return status_ok();
    }
    if (kind != config::value_kind::table) {
        return std::unexpected{
            policy_error(file, "[signature] must be a table")};
    }
    constexpr std::array<std::string_view, 2> allowed{"require", "policy"};
    if (auto known{
            reject_unknown_keys(document, path, allowed, "signature", file)};
        !known) {
        return known;
    }
    constexpr std::array<std::string_view, 2> require_path{"signature",
                                                           "require"};
    if (auto require{
            policy_bool(document, require_path, "[signature].require", file)};
        require) {
        policy.require_signature = *require;
    } else {
        return std::unexpected{std::move(require.error())};
    }
    constexpr std::array<std::string_view, 2> mode_path{"signature", "policy"};
    auto mode{policy_string(document, mode_path, "[signature].policy", file)};
    if (!mode) {
        return std::unexpected{std::move(mode.error())};
    }
    if (!mode->empty()) {
        auto parsed{parse_rfmod_signature_policy(*mode)};
        if (!parsed) {
            return std::unexpected{policy_error(
                file, fmt::format("[signature].policy must be 'strict' or "
                                  "'trusted-any', got '{}'",
                                  *mode))};
        }
        policy.signature_policy = *parsed;
    }
    return status_ok();
}

[[nodiscard]] auto read_trust_table(const config::document &document,
                                    std::string_view file,
                                    install_policy_config &policy) -> status {
    const std::array<std::string_view, 1> path{"trust"};
    const auto kind{document.kind_at(path)};
    if (kind == config::value_kind::missing) {
        return status_ok();
    }
    if (kind != config::value_kind::table) {
        return std::unexpected{policy_error(file, "[trust] must be a table")};
    }
    auto keys{document.keys_at(path)};
    if (!keys) {
        return std::unexpected{policy_error(
            file, fmt::format("[trust]: {}", keys.error().message))};
    }
    for (const auto &key_id : *keys) {
        const std::array<std::string_view, 2> anchor_path{"trust", key_id};
        auto hex{document.get_string_at(anchor_path)};
        if (!hex) {
            return std::unexpected{policy_error(
                file, fmt::format("[trust] anchor '{}' must be a public key "
                                  "hex string",
                                  key_id))};
        }
        auto public_key{data::parse_public_key_hex(*hex)};
        if (!public_key) {
            return std::unexpected{policy_error(
                file, fmt::format("[trust] anchor '{}': {}", key_id,
                                  public_key.error().message))};
        }
        policy.trusted_keys.push_back(
            rfmod_trusted_key{.key_id = key_id, .public_key = *public_key});
    }
    std::sort(policy.trusted_keys.begin(), policy.trusted_keys.end(),
              [](const rfmod_trusted_key &lhs, const rfmod_trusted_key &rhs) {
                  return lhs.key_id < rhs.key_id;
              });
    return status_ok();
}

[[nodiscard]] auto read_install_table(const config::document &document,
                                      std::string_view file,
                                      install_policy_config &policy) -> status {
    const std::array<std::string_view, 1> path{"install"};
    const auto kind{document.kind_at(path)};
    if (kind == config::value_kind::missing) {
        return status_ok();
    }
    if (kind != config::value_kind::table) {
        return std::unexpected{policy_error(file, "[install] must be a table")};
    }
    constexpr std::array<std::string_view, 3> allowed{
        "allow_downgrade", "replace_same_version", "keep_signer_pin"};
    if (auto known{
            reject_unknown_keys(document, path, allowed, "install", file)};
        !known) {
        return known;
    }
    const std::array<std::pair<std::string_view, bool *>, 3> switches{{
        {"allow_downgrade", &policy.allow_downgrade},
        {"replace_same_version", &policy.replace_same_version},
        {"keep_signer_pin", &policy.keep_signer_pin},
    }};
    for (const auto &[name, target] : switches) {
        const std::array<std::string_view, 2> switch_path{"install", name};
        auto value{policy_bool(document, switch_path, name, file)};
        if (!value) {
            return std::unexpected{std::move(value.error())};
        }
        *target = *value;
    }
    return status_ok();
}

[[nodiscard]] auto read_rotation_table(const config::document &document,
                                       std::string_view file,
                                       install_policy_config &policy)
    -> status {
    const std::array<std::string_view, 1> path{"rotation"};
    const auto kind{document.kind_at(path)};
    if (kind == config::value_kind::missing) {
        return status_ok();
    }
    if (kind != config::value_kind::table) {
        return std::unexpected{
            policy_error(file, "[rotation] must be a table")};
    }
    auto ids{document.keys_at(path)};
    if (!ids) {
        return std::unexpected{policy_error(
            file, fmt::format("[rotation]: {}", ids.error().message))};
    }
    for (const auto &id : *ids) {
        if (id.empty()) {
            return std::unexpected{
                policy_error(file, "[rotation] has an empty mod id")};
        }
        const std::array<std::string_view, 2> allowed{"old_key_id",
                                                      "new_key_id"};
        const std::array<std::string_view, 2> rotation_path{"rotation", id};
        if (auto known{reject_unknown_keys(document, rotation_path, allowed,
                                           "rotation", file)};
            !known) {
            return known;
        }
        const std::array<std::string_view, 3> old_path{"rotation", id,
                                                       "old_key_id"};
        const std::array<std::string_view, 3> new_path{"rotation", id,
                                                       "new_key_id"};
        auto old_key{policy_string(
            document, old_path, fmt::format("[rotation.\"{}\"].old_key_id", id),
            file)};
        if (!old_key) {
            return std::unexpected{std::move(old_key.error())};
        }
        auto new_key{policy_string(
            document, new_path, fmt::format("[rotation.\"{}\"].new_key_id", id),
            file)};
        if (!new_key) {
            return std::unexpected{std::move(new_key.error())};
        }
        if (old_key->empty() || new_key->empty()) {
            return std::unexpected{policy_error(
                file, fmt::format("[rotation.\"{}\"] needs non-empty "
                                  "old_key_id and new_key_id",
                                  id))};
        }
        policy.rotations.push_back(
            rfmod_key_rotation{.id = id,
                               .old_key_id = std::move(*old_key),
                               .new_key_id = std::move(*new_key)});
    }
    std::sort(policy.rotations.begin(), policy.rotations.end(),
              [](const rfmod_key_rotation &lhs, const rfmod_key_rotation &rhs) {
                  if (lhs.id != rhs.id) {
                      return lhs.id < rhs.id;
                  }
                  return lhs.old_key_id < rhs.old_key_id;
              });
    return status_ok();
}

} // namespace

auto load_install_policy(std::string_view path)
    -> result<install_policy_config> {
    if (path.empty()) {
        return std::unexpected{make_error(error_code::invalid_argument,
                                          "mods policy: empty path")};
    }
    if (!core::io::is_regular_file(path)) {
        return std::unexpected{make_error(
            error_code::not_found,
            fmt::format("mods policy: '{}' is not a regular file", path))};
    }
    auto document{config::document::parse_file(path)};
    if (!document) {
        return std::unexpected{
            policy_error(path, std::move(document.error()).message)};
    }

    auto root_keys{document->keys_at({})};
    if (!root_keys) {
        return std::unexpected{
            policy_error(path, fmt::format("cannot list the document root: {}",
                                           root_keys.error().message))};
    }
    constexpr std::array<std::string_view, 4> allowed{"signature", "trust",
                                                      "install", "rotation"};
    for (const auto &key : *root_keys) {
        if (std::find(allowed.begin(), allowed.end(), key) == allowed.end()) {
            return std::unexpected{policy_error(
                path, fmt::format("unknown top-level table '{}' (expected "
                                  "signature, trust, install or rotation)",
                                  key))};
        }
    }

    install_policy_config policy;
    policy.source = std::string{path};
    for (const auto &reader : {&read_signature_table, &read_trust_table,
                               &read_install_table, &read_rotation_table}) {
        if (auto status{reader(*document, path, policy)}; !status) {
            return std::unexpected{std::move(status.error())};
        }
    }
    return policy;
}

void apply_install_policy_overrides(install_policy_config &policy,
                                    const install_policy_overrides &overrides) {
    if (overrides.require_signature.has_value()) {
        policy.require_signature = *overrides.require_signature;
    }
    if (overrides.allow_downgrade.has_value()) {
        policy.allow_downgrade = *overrides.allow_downgrade;
    }
    if (overrides.replace_same_version.has_value()) {
        policy.replace_same_version = *overrides.replace_same_version;
    }
    if (overrides.keep_signer_pin.has_value()) {
        policy.keep_signer_pin = *overrides.keep_signer_pin;
    }
    if (overrides.signature_policy.has_value()) {
        policy.signature_policy = *overrides.signature_policy;
    }
    for (const auto &key : overrides.trusted_keys) {
        const auto known{
            std::none_of(policy.trusted_keys.begin(), policy.trusted_keys.end(),
                         [&key](const rfmod_trusted_key &existing) {
                             return existing.key_id == key.key_id;
                         })};
        if (known) {
            policy.trusted_keys.push_back(key);
        }
    }
    for (const auto &rotation : overrides.rotations) {
        policy.rotations.push_back(rotation);
    }
}

void apply_install_policy(const install_policy_config &policy,
                          rfmod_install_options &options) {
    options.require_signature = policy.require_signature;
    options.trusted_keys = policy.trusted_keys;
    options.signature_policy = policy.signature_policy;
    options.rotations = policy.rotations;
    options.allow_downgrade = policy.allow_downgrade;
    options.replace_same_version = policy.replace_same_version;
}

auto format_install_policy(const install_policy_config &policy) -> std::string {
    std::string out;
    out += fmt::format("policy: {} (require signature: {})\n",
                       to_string(policy.signature_policy),
                       policy.require_signature ? "yes" : "no");
    out += fmt::format("install: allow downgrade {}, replace same version {}, "
                       "keep signer pin {}\n",
                       policy.allow_downgrade ? "yes" : "no",
                       policy.replace_same_version ? "yes" : "no",
                       policy.keep_signer_pin ? "yes" : "no");
    for (const auto &key : policy.trusted_keys) {
        out += fmt::format("trust anchor: {}={}\n", key.key_id,
                           data::to_hex(key.public_key));
    }
    for (const auto &rotation : policy.rotations) {
        out += fmt::format("rotation: {} {} -> {}\n", rotation.id,
                           rotation.old_key_id, rotation.new_key_id);
    }
    return out;
}

} // namespace refrain::mod
