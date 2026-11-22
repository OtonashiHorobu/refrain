// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/app/mod_management.hpp>
#include <refrain/app/services.hpp>

#include <refrain/core/io.hpp>
#include <refrain/core/log.hpp>
#include <refrain/core/semver.hpp>
#include <refrain/core/version.hpp>
#include <refrain/data/checksum.hpp>
#include <refrain/data/content_store.hpp>
#include <refrain/mod/discovery.hpp>
#include <refrain/mod/mod_ref.hpp>
#include <refrain/mod/rfmod.hpp>

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

constexpr refrain::log::logger log_audit{"mod-audit"};

constexpr std::string_view installed_suffix{".rfmod"};

[[nodiscard]] auto hex(const refrain::data::digest32 &digest) -> std::string {
    return refrain::data::to_hex(digest);
}

[[nodiscard]] auto short_hex(const refrain::data::digest32 &digest)
    -> std::string {
    auto text{hex(digest)};
    text.resize(12);
    return text;
}

[[nodiscard]] auto join(const std::vector<std::string> &values,
                        std::string_view separator) -> std::string {
    std::string out{};
    for (const auto &value : values) {
        if (!out.empty()) {
            out += separator;
        }
        out += value;
    }
    return out;
}

/// Signature policy text recorded in receipts and audit records: an unsigned
/// package records `unsigned`, a signed one records the continuity policy in
/// force.
[[nodiscard]] auto policy_text(bool signed_package,
                               refrain::mod::rfmod_signature_policy policy)
    -> std::string {
    if (!signed_package) {
        return "unsigned";
    }
    return std::string{refrain::mod::to_string(policy)};
}

/// `id` or `id@version`. The version is the exact installed version text, not a
/// range; an empty trailing version is rejected.
struct mod_reference {
    std::string id{};
    std::string version{};
};

[[nodiscard]] auto parse_mod_reference(std::string_view text)
    -> refrain::result<mod_reference> {
    if (text.empty()) {
        return std::unexpected{refrain::make_error(
            refrain::error_code::invalid_argument, "a mod id is required")};
    }
    const auto at{text.rfind('@')};
    if (at == std::string_view::npos) {
        return mod_reference{.id = std::string{text}};
    }
    if (at == 0 || at + 1 >= text.size()) {
        return std::unexpected{refrain::make_error(
            refrain::error_code::invalid_argument,
            fmt::format("'{}' is not <id>[@<version>]", text))};
    }
    return mod_reference{.id = std::string{text.substr(0, at)},
                         .version = std::string{text.substr(at + 1U)}};
}

/// A package path is operator input read only: reject anything that is not an
/// existing regular `.rfmod` file before the library opens it.
[[nodiscard]] auto package_path_problem(std::string_view path)
    -> std::optional<std::string> {
    if (path.empty()) {
        return std::string{"a package path is required"};
    }
    if (!path.ends_with(installed_suffix)) {
        return fmt::format("'{}' is not an .rfmod package", path);
    }
    if (!refrain::core::io::exists(path)) {
        return fmt::format("'{}' does not exist", path);
    }
    if (!refrain::core::io::is_regular_file(path)) {
        return fmt::format("'{}' is not a regular file", path);
    }
    return std::nullopt;
}

[[nodiscard]] auto refusal(std::string message,
                           std::string_view outcome = "failed")
    -> refrain::app::mod_management_report {
    refrain::app::mod_management_report report{};
    report.ok = false;
    report.error = std::move(message);
    report.outcome = std::string{outcome};
    return report;
}

[[nodiscard]] auto success(std::vector<std::string> lines)
    -> refrain::app::mod_management_report {
    refrain::app::mod_management_report report{};
    report.lines = std::move(lines);
    return report;
}

/// One parsed management line: `mods <verb> [positional...] [--dry-run]
/// [confirm]`. Flags are recognised anywhere after the verb.
struct parsed_line {
    std::string_view verb{};
    std::vector<std::string_view> positional{};
    bool dry_run{false};
    bool confirm{false};
};

[[nodiscard]] auto parse_line(std::span<const std::string_view> arguments)
    -> refrain::result<parsed_line> {
    if (arguments.empty()) {
        return std::unexpected{
            refrain::make_error(refrain::error_code::invalid_argument, "")};
    }
    parsed_line line{};
    line.verb = arguments.front();
    for (std::size_t index{1}; index < arguments.size(); ++index) {
        const auto token{arguments[index]};
        if (token == "--dry-run") {
            line.dry_run = true;
        } else if (token == "confirm") {
            line.confirm = true;
        } else if (token.starts_with("--")) {
            return std::unexpected{
                refrain::make_error(refrain::error_code::invalid_argument,
                                    fmt::format("unknown option '{}'", token))};
        } else {
            line.positional.push_back(token);
        }
    }
    return line;
}

/// Usage printed by `mods` without a subcommand and by `mods help`.
constexpr std::string_view usage_text{
    "the available mod commands are: list, installed, verify <package>, "
    "install <package> [confirm], uninstall <id>[@<version>] [confirm], "
    "reload <id>[@<version>], reload-data, trust [list], trust pin <id> "
    "[confirm], trust clear <id> [confirm]; destructive commands need "
    "'confirm' and install, uninstall, reload-data and trust accept "
    "'--dry-run'"};

[[nodiscard]] auto usage_report() -> refrain::app::mod_management_report {
    return success({std::string{usage_text}});
}

[[nodiscard]] auto confirmation_error(std::string_view action,
                                      std::string_view subject)
    -> refrain::app::mod_management_report {
    return refusal(
        fmt::format("refusing to {} {} without confirmation; repeat the "
                    "command with 'confirm' to apply it",
                    action, subject),
        "refused");
}

/// Sorted installed receipts, ordered by id and then SemVer precedence (ties
/// broken by the raw version text so the order is total).
struct sorted_receipt {
    refrain::mod::mod_receipt receipt{};
    refrain::core::semver precedence{};
};

[[nodiscard]] auto read_installed_receipts(std::string_view store)
    -> refrain::result<std::vector<sorted_receipt>> {
    auto paths{refrain::mod::list_receipt_paths(store)};
    if (!paths) {
        return std::unexpected{paths.error()};
    }
    std::vector<sorted_receipt> receipts{};
    receipts.reserve(paths->size());
    for (const auto &path : *paths) {
        auto receipt{refrain::mod::read_receipt(path)};
        if (!receipt) {
            return std::unexpected{receipt.error()};
        }
        auto precedence{refrain::core::semver::parse(receipt->version)};
        if (!precedence) {
            return std::unexpected{precedence.error()};
        }
        receipts.push_back(
            sorted_receipt{.receipt = std::move(*receipt),
                           .precedence = std::move(*precedence)});
    }
    std::sort(receipts.begin(), receipts.end(),
              [](const sorted_receipt &lhs, const sorted_receipt &rhs) {
                  if (lhs.receipt.id != rhs.receipt.id) {
                      return lhs.receipt.id < rhs.receipt.id;
                  }
                  const auto order{
                      refrain::core::compare(lhs.precedence, rhs.precedence)};
                  if (order != std::strong_ordering::equal) {
                      return order == std::strong_ordering::less;
                  }
                  return lhs.receipt.version < rhs.receipt.version;
              });
    return receipts;
}

/// Highest-precedence installed receipt of one id, or a `not_found` failure
/// naming the id.
[[nodiscard]] auto latest_receipt(std::string_view store, std::string_view id)
    -> refrain::result<refrain::mod::mod_receipt> {
    auto receipts{read_installed_receipts(store)};
    if (!receipts) {
        return std::unexpected{receipts.error()};
    }
    const refrain::mod::mod_receipt *latest{nullptr};
    for (const auto &entry : *receipts) {
        if (entry.receipt.id == id) {
            latest = &entry.receipt;
        }
    }
    if (latest == nullptr) {
        return std::unexpected{
            refrain::make_error(refrain::error_code::not_found,
                                fmt::format("mod '{}' is not installed", id))};
    }
    return *latest;
}

/// Verify one opened package against the configured install policy and copy
/// its audit identity into `audit`.
[[nodiscard]] auto
verify_package(const refrain::mod::rfmod_reader &reader,
               const refrain::app::mod_management_context &context,
               refrain::app::mod_audit_record &audit)
    -> refrain::result<refrain::mod::rfmod_verification> {
    refrain::mod::rfmod_verify_options options{};
    options.trusted_keys = context.policy.trusted_keys;
    options.require_signature = context.policy.require_signature;
    auto verified{refrain::mod::verify_rfmod(reader, options)};
    if (!verified) {
        audit.digest = hex(reader.archive_digest());
        return std::unexpected{verified.error()};
    }
    audit.id = verified->manifest.common.id;
    audit.version = verified->manifest.common.version;
    audit.digest = hex(verified->archive_digest);
    audit.key_id = verified->key_id;
    audit.policy =
        policy_text(verified->signed_package, context.policy.signature_policy);
    return verified;
}

[[nodiscard]] auto
verify_lines(const refrain::mod::rfmod_reader &reader,
             const refrain::mod::rfmod_verification &verified)
    -> std::vector<std::string> {
    std::vector<std::string> lines{};
    const auto &common{verified.manifest.common};
    lines.push_back(fmt::format("{} {} is a valid package with {} file(s)",
                                common.id, common.version,
                                reader.file_count()));
    lines.push_back(
        fmt::format("archive blake3 is {}", hex(verified.archive_digest)));
    lines.push_back(
        fmt::format("manifest blake3 is {}", hex(verified.manifest_digest)));
    if (verified.signed_package) {
        lines.push_back(fmt::format(
            "signature is ed25519 with key id '{}', {}", verified.key_id,
            verified.trusted ? "trusted" : "not trusted"));
    } else {
        lines.push_back("signature is absent, the package is unsigned");
    }
    return lines;
}

namespace sync_detail {

using refrain::core::semver_detail::trim;

/// Which sync-managed section a TOML table header opens. A header under
/// `[mods."<id>"]` is deliberately not the mods list: its `requires` array
/// mirrors a manifest and must not be rewritten by a version sync.
[[nodiscard]] auto section_of(std::string_view header)
    -> std::optional<refrain::app::gamepack_sync_section> {
    header = trim(header);
    if (header == "mods") {
        return refrain::app::gamepack_sync_section::mods_list;
    }
    if (header == "providers" || header.starts_with("providers.")) {
        return refrain::app::gamepack_sync_section::providers;
    }
    return std::nullopt;
}

/// Rebase a simple SemVer range onto the installed version. A compound range
/// (an AND group with bounds, or an `||` alternative) is left untouched: its
/// shape carries a decision a version swap cannot express.
[[nodiscard]] auto rebase_range(std::string_view range,
                                std::string_view version)
    -> std::optional<std::string> {
    constexpr std::array<std::string_view, 7> operators{">=", "<=", "^", "~",
                                                        ">",  "<",  "="};
    std::string_view prefix{};
    for (const auto op : operators) {
        if (range.starts_with(op)) {
            prefix = op;
            break;
        }
    }
    const auto bound{trim(range.substr(prefix.size()))};
    if (!refrain::core::semver::parse(bound) ||
        !refrain::core::semver::parse(version)) {
        return std::nullopt;
    }
    return fmt::format("{}{}", prefix, version);
}

template <class Function>
void for_each_requirement(const refrain::mod::provider_binding &binding,
                          Function &&function) {
    for (const auto &candidate : binding.candidates) {
        function(candidate);
    }
    for (const auto &instance : binding.instances) {
        function(instance.requirement);
    }
    if (binding.base) {
        function(*binding.base);
    }
    for (const auto &wrapper : binding.wrappers) {
        function(wrapper);
    }
}

} // namespace sync_detail

} // namespace

namespace refrain::app {

auto format_mod_audit_record(const mod_audit_record &record) -> std::string {
    std::string out{record.actor_kind.empty() ? "unknown" : record.actor_kind};
    if (!record.actor_uuid.empty()) {
        out += fmt::format(" {} ({})", record.actor_uuid,
                           record.actor_name.empty() ? "unnamed"
                                                     : record.actor_name);
    } else if (!record.actor_name.empty()) {
        out += fmt::format(" {}", record.actor_name);
    }
    out += fmt::format(" ran {} on", record.action);
    if (!record.id.empty()) {
        out += fmt::format(" {} {}", record.id, record.version);
    } else {
        out += " the mod catalog";
    }
    if (!record.digest.empty()) {
        out += fmt::format(" from archive {}", record.digest);
    }
    if (!record.key_id.empty()) {
        out += fmt::format(" signed by key '{}'", record.key_id);
    }
    if (!record.policy.empty()) {
        out += fmt::format(" under the {} policy", record.policy);
    }
    out += fmt::format(", the result was {}", record.result);
    if (!record.detail.empty()) {
        out += fmt::format(": {}", record.detail);
    }
    out += fmt::format(" after {}ms", record.duration_ms);
    return out;
}

mod_management::mod_management(mod_management_context context)
    : context_{std::move(context)} {}

namespace {

class command_runner {
  public:
    command_runner(const mod_management_context &context,
                   mod_audit_record &audit)
        : context_{context}, audit_{audit} {}

    [[nodiscard]] auto run(std::span<const std::string_view> arguments)
        -> mod_management_report {
        if (arguments.empty() || arguments.front() == "help") {
            audit_.action = "help";
            return usage_report();
        }
        auto line{parse_line(arguments)};
        if (!line) {
            audit_.action = std::string{arguments.front()};
            return refusal(line.error().message);
        }
        audit_.action = std::string{line->verb};
        if (line->verb == "list") {
            return list(*line);
        }
        if (line->verb == "installed") {
            return installed(*line);
        }
        if (line->verb == "verify") {
            return verify(*line);
        }
        if (line->verb == "install") {
            return install(*line);
        }
        if (line->verb == "uninstall") {
            return uninstall(*line);
        }
        if (line->verb == "reload") {
            return reload(*line);
        }
        if (line->verb == "reload-data") {
            return reload_data(*line);
        }
        if (line->verb == "trust") {
            return trust(*line);
        }
        return refusal(fmt::format("unknown mod command '{}'; {}", line->verb,
                                   usage_text));
    }

  private:
    [[nodiscard]] auto list(const parsed_line &line) -> mod_management_report {
        if (!line.positional.empty()) {
            return refusal("list takes no arguments");
        }
        std::vector<std::string> lines{};
        if (context_.loaded.empty()) {
            lines.push_back("no mods are part of the running composition");
            return success(std::move(lines));
        }
        lines.push_back(fmt::format("the running composition holds {} mods",
                                    context_.loaded.size()));
        for (const auto &mod : context_.loaded) {
            lines.push_back(fmt::format(
                "{} {} is loaded by the {} loader with {}, from {}, {}", mod.id,
                mod.version, mod.loader.empty() ? "unknown" : mod.loader,
                mod.has_code ? "code" : "data only", mod.source,
                mod.services.empty()
                    ? std::string{"no service"}
                    : fmt::format("service {}", join(mod.services, ", "))));
        }
        return success(std::move(lines));
    }

    [[nodiscard]] auto installed(const parsed_line &line)
        -> mod_management_report {
        if (!line.positional.empty()) {
            return refusal("installed takes no arguments");
        }
        if (context_.store.empty()) {
            return refusal(
                "no content store is configured; start the server with "
                "--content-store <dir> to manage installed mods");
        }
        auto receipts{read_installed_receipts(context_.store)};
        if (!receipts) {
            return refusal(receipts.error().message);
        }
        std::vector<std::string> lines{};
        if (receipts->empty()) {
            lines.push_back(
                fmt::format("no mods are installed in '{}'", context_.store));
            return success(std::move(lines));
        }
        lines.push_back(fmt::format("{} mod(s) are installed in '{}'",
                                    receipts->size(), context_.store));
        for (const auto &entry : *receipts) {
            const auto &receipt{entry.receipt};
            const auto trust{
                receipt.signed_package
                    ? (receipt.trusted ? "signed trusted" : "signed untrusted")
                    : "unsigned"};
            lines.push_back(fmt::format(
                "{} {} from archive {} is {}, {}, receipt policy {}",
                receipt.id, receipt.version, short_hex(receipt.archive_blake3),
                trust,
                receipt.key_id.empty()
                    ? std::string{"with no recorded signer"}
                    : fmt::format("signed by key '{}'", receipt.key_id),
                receipt.policy));
        }
        return success(std::move(lines));
    }

    [[nodiscard]] auto verify(const parsed_line &line)
        -> mod_management_report {
        if (line.positional.size() != 1) {
            return refusal("verify expects one .rfmod package path");
        }
        const auto path{line.positional.front()};
        if (const auto problem{package_path_problem(path)}; problem) {
            return refusal(*problem);
        }
        auto reader{refrain::mod::rfmod_reader::open_file(path)};
        if (!reader) {
            return refusal(reader.error().message);
        }
        auto verified{verify_package(*reader, context_, audit_)};
        if (!verified) {
            return refusal(verified.error().message);
        }
        auto lines{verify_lines(*reader, *verified)};
        auto report{success(std::move(lines))};
        report.outcome = line.dry_run ? "dry-run" : "ok";
        return report;
    }

    [[nodiscard]] auto install(const parsed_line &line)
        -> mod_management_report {
        if (line.positional.size() != 1) {
            return refusal("install expects one .rfmod package path");
        }
        const auto path{line.positional.front()};
        if (const auto problem{package_path_problem(path)}; problem) {
            return refusal(*problem);
        }
        if (!line.confirm && !line.dry_run) {
            return confirmation_error("install", path);
        }
        auto reader{refrain::mod::rfmod_reader::open_file(path)};
        if (!reader) {
            return refusal(reader.error().message);
        }
        auto verified{verify_package(*reader, context_, audit_)};
        if (!verified) {
            return refusal(verified.error().message);
        }
        const auto &common{verified->manifest.common};
        const auto subject{fmt::format("{} {}", common.id, common.version)};
        if (line.dry_run) {
            auto lines{verify_lines(*reader, *verified)};
            lines.push_back(fmt::format(
                "dry run: would install {} into {}; no store state was "
                "written and the running composition is unchanged",
                subject,
                context_.store.empty()
                    ? std::string{"the content store, but none is configured"}
                    : fmt::format("'{}'", context_.store)));
            auto report{success(std::move(lines))};
            report.outcome = "dry-run";
            return report;
        }
        if (context_.store.empty()) {
            return refusal(
                "no content store is configured; start the server with "
                "--content-store <dir> to install mods");
        }
        auto store{refrain::data::content_store::open(context_.store)};
        if (!store) {
            return refusal(store.error().message);
        }
        refrain::mod::rfmod_install_options options{};
        refrain::mod::apply_install_policy(context_.policy, options);
        options.write_receipt = true;
        options.installer_version =
            std::string{refrain::version::release_version};
        auto result{refrain::mod::install_rfmod(*reader, *store, options)};
        if (!result) {
            return refusal(result.error().message);
        }
        auto lines{install_lines(*result, context_.store)};
        auto report{activate(mod_lifecycle_action::install, common.id,
                             common.version, std::string{path},
                             std::move(lines))};
        return report;
    }

    [[nodiscard]] auto uninstall(const parsed_line &line)
        -> mod_management_report {
        if (line.positional.size() != 1) {
            return refusal("uninstall expects one <id>[@<version>] reference");
        }
        auto reference{parse_mod_reference(line.positional.front())};
        if (!reference) {
            return refusal(reference.error().message);
        }
        audit_.id = reference->id;
        audit_.version = reference->version;
        if (context_.store.empty()) {
            return refusal(
                "no content store is configured; start the server with "
                "--content-store <dir> to uninstall mods");
        }
        if (!line.confirm && !line.dry_run) {
            return confirmation_error("uninstall",
                                      format_reference(*reference));
        }
        if (line.dry_run) {
            auto paths{refrain::mod::list_receipt_paths_for(context_.store,
                                                            reference->id)};
            if (!paths) {
                return refusal(paths.error().message);
            }
            std::vector<std::string> versions{};
            for (const auto &path : *paths) {
                auto receipt{refrain::mod::read_receipt(path)};
                if (!receipt) {
                    return refusal(receipt.error().message);
                }
                if (reference->version.empty() ||
                    reference->version == receipt->version) {
                    versions.push_back(receipt->version);
                }
            }
            if (versions.empty()) {
                return refusal(
                    fmt::format("mod '{}' is not installed", reference->id),
                    "failed");
            }
            std::vector<std::string> lines{};
            for (const auto &version : versions) {
                lines.push_back(fmt::format(
                    "dry run: would uninstall {} {} from '{}'; no store "
                    "state was written and the running composition is "
                    "unchanged",
                    reference->id, version, context_.store));
            }
            auto report{success(std::move(lines))};
            report.outcome = "dry-run";
            return report;
        }
        auto store{refrain::data::content_store::open_existing(context_.store)};
        if (!store) {
            return refusal(store.error().message);
        }
        refrain::mod::rfmod_uninstall_options options{};
        std::optional<std::string_view> version{};
        if (!reference->version.empty()) {
            version = reference->version;
        }
        auto removed{refrain::mod::uninstall_rfmod(*store, reference->id,
                                                   version, options)};
        if (!removed) {
            return refusal(removed.error().message);
        }
        std::vector<std::string> lines{};
        for (const auto &removed_version : removed->versions) {
            lines.push_back(fmt::format("uninstalled {} {} from '{}'",
                                        removed->id, removed_version,
                                        context_.store));
            if (audit_.version.empty()) {
                audit_.version = removed_version;
            }
        }
        if (removed->last_version) {
            lines.push_back(
                "the signer pin was cleared because no installed version "
                "remains");
        }
        lines.push_back(
            fmt::format("the removed blobs stay in the content store until "
                        "`refrain-tools content gc --store {} --apply` "
                        "collects them",
                        context_.store));
        if (!needs_live_removal(reference->id, removed->versions)) {
            lines.push_back(fmt::format(
                "the running composition does not load {} {} from the store, "
                "so no restart is required",
                reference->id,
                removed->versions.size() == 1U
                    ? removed->versions.front()
                    : std::string{"at the removed version"}));
            return success(std::move(lines));
        }
        return activate(mod_lifecycle_action::uninstall, reference->id,
                        reference->version, {}, std::move(lines));
    }

    [[nodiscard]] auto reload(const parsed_line &line)
        -> mod_management_report {
        if (line.positional.size() != 1) {
            return refusal("reload expects one <id>[@<version>] reference");
        }
        auto reference{parse_mod_reference(line.positional.front())};
        if (!reference) {
            return refusal(reference.error().message);
        }
        audit_.id = reference->id;
        audit_.version = reference->version;
        const auto *loaded{find_loaded(reference->id)};
        if (loaded == nullptr) {
            return refusal(fmt::format(
                "mod '{}' is not part of the running composition; reload "
                "can only rebuild a mod that is already loaded",
                reference->id));
        }
        const auto version{reference->version.empty() ? loaded->version
                                                      : reference->version};
        audit_.version = version;
        if (version != loaded->version) {
            return refusal(fmt::format(
                "the running composition holds {} {}; reload cannot switch "
                "to version {}",
                loaded->id, loaded->version, version));
        }
        if (!context_.hot_reload) {
            return refusal(
                fmt::format(
                    "hot reload is disabled ([lifecycle] hot_reload = false "
                    "in '{}'); set it to true and restart the server to "
                    "reload {} {}",
                    admin_source(), loaded->id, version),
                "refused");
        }
        if (!context_.lifecycle.apply) {
            return refusal(fmt::format("cannot reload {} {} at runtime: {}",
                                       loaded->id, version, lifecycle_reason()),
                           "unsupported");
        }
        if (line.dry_run) {
            auto report{success({fmt::format(
                "dry run: would reload {} {} through the hot lifecycle "
                "registry; the running composition is unchanged",
                loaded->id, version)})};
            report.outcome = "dry-run";
            return report;
        }
        mod_lifecycle_request request{};
        request.action = mod_lifecycle_action::reload;
        request.id = loaded->id;
        request.version = version;
        auto applied{context_.lifecycle.apply(request)};
        if (!applied) {
            return refusal(applied.error().message);
        }
        auto lines{applied->lines};
        if (lines.empty()) {
            lines.push_back(
                fmt::format("reloaded {} {} in the running composition",
                            loaded->id, version));
        }
        return success(std::move(lines));
    }

    /// Rebuild the datapack content view (recipes, loot tables, advancements,
    /// worldgen documents, assets) without touching the mod composition. The
    /// build and validation run off the tick; the new view publishes at the
    /// next tick boundary and a failed build leaves the live view in place.
    [[nodiscard]] auto reload_data(const parsed_line &line)
        -> mod_management_report {
        if (!line.positional.empty()) {
            return refusal("reload-data takes no arguments");
        }
        if (!context_.hot_reload_data) {
            return refusal(
                fmt::format(
                    "hot content reload is disabled ([lifecycle] "
                    "hot_reload_data = false in '{}'); set it to true and "
                    "restart the server to reload content at runtime",
                    admin_source()),
                "refused");
        }
        if (!context_.content_reload.request) {
            return refusal(fmt::format("cannot reload content at runtime: {}",
                                       content_reload_reason()),
                           "unsupported");
        }
        if (line.dry_run) {
            auto report{success({std::string{
                "dry run: would rebuild the content view from its layer list "
                "and validate it off the tick; the running view is "
                "unchanged"}})};
            report.outcome = "dry-run";
            return report;
        }
        auto applied{context_.content_reload.request()};
        if (!applied) {
            return refusal(applied.error().message);
        }
        return success(std::move(applied->lines));
    }

    [[nodiscard]] auto trust(const parsed_line &line) -> mod_management_report {
        if (line.positional.empty() || line.positional.front() == "list") {
            if (line.positional.size() > 1) {
                return refusal("trust list takes no arguments");
            }
            audit_.action = "trust-list";
            return trust_list();
        }
        const auto verb{line.positional.front()};
        if (verb == "pin") {
            audit_.action = "trust-pin";
            return trust_pin(line);
        }
        if (verb == "clear" || verb == "unpin") {
            audit_.action = "trust-clear";
            return trust_clear(line);
        }
        return refusal(
            fmt::format("unknown trust command '{}'; use trust list, trust "
                        "pin <id> or trust clear <id>",
                        verb));
    }

    [[nodiscard]] auto trust_list() -> mod_management_report {
        std::vector<std::string> lines{};
        if (!context_.policy_source.empty()) {
            lines.push_back(fmt::format("the install policy is read from '{}'",
                                        context_.policy_source));
        }
        lines.push_back(fmt::format(
            "the install policy is {} and requires a trusted signature: {}",
            refrain::mod::to_string(context_.policy.signature_policy),
            context_.policy.require_signature ? "yes" : "no"));
        if (context_.policy.trusted_keys.empty()) {
            lines.push_back("no trust anchors are configured");
        } else {
            for (const auto &key : context_.policy.trusted_keys) {
                lines.push_back(fmt::format(
                    "trust anchor '{}' fingerprints as {}", key.key_id,
                    refrain::mod::public_key_fingerprint(key.public_key)));
            }
        }
        for (const auto &rotation : context_.policy.rotations) {
            lines.push_back(fmt::format(
                "signer rotation for {} allows '{}' to '{}'", rotation.id,
                rotation.old_key_id, rotation.new_key_id));
        }
        if (context_.store.empty()) {
            lines.push_back(
                "no content store is configured, so signer pins are "
                "unavailable");
            return success(std::move(lines));
        }
        auto receipts{read_installed_receipts(context_.store)};
        if (!receipts) {
            return refusal(receipts.error().message);
        }
        std::vector<std::string> ids{};
        for (const auto &entry : *receipts) {
            if (ids.empty() || ids.back() != entry.receipt.id) {
                ids.push_back(entry.receipt.id);
            }
        }
        if (ids.empty()) {
            lines.push_back(
                fmt::format("no signer pins exist in '{}'", context_.store));
            return success(std::move(lines));
        }
        for (const auto &id : ids) {
            auto pin{refrain::mod::read_signer_pin(context_.store, id)};
            if (!pin) {
                return refusal(pin.error().message);
            }
            if (!pin->has_value()) {
                lines.push_back(fmt::format(
                    "no signer pin is recorded for {}, the next update "
                    "establishes one",
                    id));
                continue;
            }
            lines.push_back(fmt::format(
                "signer pin for {} is key '{}'{} under the {} policy", id,
                (**pin).key_id,
                (**pin).public_key_fingerprint.empty()
                    ? std::string{}
                    : fmt::format(" with fingerprint {}",
                                  (**pin).public_key_fingerprint),
                refrain::mod::to_string((**pin).policy)));
        }
        return success(std::move(lines));
    }

    [[nodiscard]] auto trust_pin(const parsed_line &line)
        -> mod_management_report {
        if (line.positional.size() != 2) {
            return refusal("trust pin expects a mod id");
        }
        auto reference{parse_mod_reference(line.positional[1])};
        if (!reference || !reference->version.empty()) {
            return refusal("trust pin expects a mod id without a version");
        }
        audit_.id = reference->id;
        if (context_.store.empty()) {
            return refusal(
                "no content store is configured; start the server with "
                "--content-store <dir> to manage signer pins");
        }
        if (!line.confirm && !line.dry_run) {
            return confirmation_error("pin the signer of", reference->id);
        }
        auto receipt{latest_receipt(context_.store, reference->id)};
        if (!receipt) {
            return refusal(receipt.error().message);
        }
        if (!receipt->signed_package || receipt->key_id.empty()) {
            return refusal(fmt::format(
                "the installed versions of '{}' are unsigned; there is no "
                "signer to pin",
                reference->id));
        }
        refrain::mod::mod_signer_pin pin{};
        pin.id = reference->id;
        pin.key_id = receipt->key_id;
        pin.public_key_fingerprint = receipt->public_key_fingerprint;
        pin.policy = context_.policy.signature_policy;
        pin.source = "admin";
        audit_.version = receipt->version;
        audit_.key_id = receipt->key_id;
        audit_.policy = policy_text(true, pin.policy);
        if (line.dry_run) {
            auto report{success({fmt::format(
                "dry run: would pin {} {} to key '{}'; no store state was "
                "written",
                reference->id, receipt->version, pin.key_id)})};
            report.outcome = "dry-run";
            return report;
        }
        if (auto written{refrain::mod::write_signer_pin(context_.store, pin)};
            !written) {
            return refusal(written.error().message);
        }
        return success(
            {fmt::format("pinned {} to key '{}'{}", reference->id, pin.key_id,
                         pin.public_key_fingerprint.empty()
                             ? std::string{}
                             : fmt::format(" with fingerprint {}",
                                           pin.public_key_fingerprint))});
    }

    [[nodiscard]] auto trust_clear(const parsed_line &line)
        -> mod_management_report {
        if (line.positional.size() != 2) {
            return refusal("trust clear expects a mod id");
        }
        auto reference{parse_mod_reference(line.positional[1])};
        if (!reference || !reference->version.empty()) {
            return refusal("trust clear expects a mod id without a version");
        }
        audit_.id = reference->id;
        if (context_.store.empty()) {
            return refusal(
                "no content store is configured; start the server with "
                "--content-store <dir> to manage signer pins");
        }
        if (!line.confirm && !line.dry_run) {
            return confirmation_error("clear the signer pin of", reference->id);
        }
        auto pin{refrain::mod::read_signer_pin(context_.store, reference->id)};
        if (!pin) {
            return refusal(pin.error().message);
        }
        if (!pin->has_value()) {
            return refusal(
                fmt::format("no signer pin is recorded for {}", reference->id));
        }
        audit_.key_id = (**pin).key_id;
        audit_.version =
            latest_receipt(context_.store, reference->id)
                .transform([](const auto &receipt) { return receipt.version; })
                .value_or(std::string{});
        if (line.dry_run) {
            auto report{success({fmt::format(
                "dry run: would clear the signer pin of {}; no store state "
                "was written",
                reference->id)})};
            report.outcome = "dry-run";
            return report;
        }
        if (auto cleared{
                refrain::mod::clear_signer_pin(context_.store, reference->id)};
            !cleared) {
            return refusal(cleared.error().message);
        }
        return success({fmt::format(
            "cleared the signer pin of {}; the next signed install "
            "establishes a new one",
            reference->id)});
    }

    /// Copy the install report lines a successful install produces.
    [[nodiscard]] auto
    install_lines(const refrain::mod::rfmod_install_result &installed,
                  std::string_view store) -> std::vector<std::string> {
        const auto &common{installed.manifest.common};
        std::vector<std::string> lines{};
        lines.push_back(fmt::format("installed {} {} into '{}'", common.id,
                                    common.version, store));
        lines.push_back(fmt::format(
            "the package has {} file(s), {} new blob(s) and {} stored "
            "blob(s), archive blake3 is {}",
            installed.files.size(), installed.new_blobs, installed.stored_blobs,
            hex(installed.archive_digest)));
        if (installed.signed_package) {
            lines.push_back(fmt::format(
                "the package is signed by key '{}', with {} trust",
                installed.key_id,
                installed.trusted ? "a configured anchor" : "no anchor"));
        } else {
            lines.push_back("the package is unsigned");
        }
        lines.push_back(fmt::format("the receipt records the {} policy at '{}'",
                                    installed.receipt_policy,
                                    installed.receipt_path));
        if (installed.signer_pinned) {
            lines.push_back(
                fmt::format("the install established the signer pin at '{}'",
                            installed.signer_pin_path));
        } else if (installed.signer_rotated) {
            lines.push_back(
                "the signer rotation was accepted by the allowlist");
        }
        if (installed.downgrade_override) {
            lines.push_back("a downgrade was accepted for this run");
        }
        if (installed.same_version_override) {
            lines.push_back("the same version was replaced for this run");
        }
        if (installed.already_installed) {
            lines.push_back(
                "the package was already installed, so nothing changed");
        }
        audit_.key_id = installed.key_id;
        audit_.policy = installed.receipt_policy;
        return lines;
    }

    /// Hand a durable composition change to the hot lifecycle implementation,
    /// or explain why it waits for a restart. `lines` already holds the
    /// store-level report.
    [[nodiscard]] auto activate(mod_lifecycle_action action,
                                const std::string &id,
                                const std::string &version, std::string package,
                                std::vector<std::string> lines)
        -> mod_management_report {
        const auto subject{version.empty() ? id
                                           : fmt::format("{} {}", id, version)};
        if (!context_.hot_install) {
            if (action == mod_lifecycle_action::uninstall) {
                lines.push_back(fmt::format(
                    "restart required: {} stays loaded because hot install "
                    "is disabled ([lifecycle] hot_install = false in '{}')",
                    id, admin_source()));
            } else {
                lines.push_back(fmt::format(
                    "restart required: {} becomes part of the composition on "
                    "the next start because hot install is disabled "
                    "([lifecycle] hot_install = false in '{}')",
                    subject, admin_source()));
            }
            auto report{success(std::move(lines))};
            report.outcome = "restart-required";
            return report;
        }
        if (!context_.lifecycle.apply) {
            auto report{success(std::move(lines))};
            report.ok = false;
            report.outcome = "unsupported";
            report.error = fmt::format(
                "the store change for {} committed, but the running "
                "composition was not changed: {}; restart the server to "
                "apply it",
                subject, lifecycle_reason());
            return report;
        }
        mod_lifecycle_request request{};
        request.action = action;
        request.id = id;
        request.version = version;
        request.package = std::move(package);
        auto applied{context_.lifecycle.apply(request)};
        if (!applied) {
            auto report{success(std::move(lines))};
            report.ok = false;
            report.outcome = "failed";
            report.error = fmt::format(
                "the store change for {} committed, but the running "
                "composition rejected it: {}",
                subject, applied.error().message);
            return report;
        }
        for (const auto &line : applied->lines) {
            lines.push_back(line);
        }
        return success(std::move(lines));
    }

    /// True when the running composition loads one of the removed versions
    /// from the store and therefore has to retire it.
    [[nodiscard]] auto
    needs_live_removal(std::string_view id,
                       const std::vector<std::string> &removed_versions) const
        -> bool {
        const auto *loaded{find_loaded(id)};
        if (loaded == nullptr || loaded->source != "installed") {
            return false;
        }
        return std::find(removed_versions.begin(), removed_versions.end(),
                         loaded->version) != removed_versions.end();
    }

    [[nodiscard]] auto find_loaded(std::string_view id) const
        -> const mod_management_mod * {
        const auto found{std::find_if(
            context_.loaded.begin(), context_.loaded.end(),
            [id](const mod_management_mod &mod) { return mod.id == id; })};
        return found == context_.loaded.end() ? nullptr : &*found;
    }

    [[nodiscard]] auto format_reference(const mod_reference &reference) const
        -> std::string {
        return reference.version.empty()
                   ? reference.id
                   : fmt::format("{} {}", reference.id, reference.version);
    }

    [[nodiscard]] auto admin_source() const -> std::string {
        return context_.admin_source.empty() ? "the mod administration "
                                               "configuration"
                                             : context_.admin_source;
    }

    [[nodiscard]] auto lifecycle_reason() const -> std::string {
        return context_.lifecycle.unavailable_reason.empty()
                   ? std::string{"the hot lifecycle registry is not available "
                                 "in this build"}
                   : context_.lifecycle.unavailable_reason;
    }

    [[nodiscard]] auto content_reload_reason() const -> std::string {
        return context_.content_reload.unavailable_reason.empty()
                   ? std::string{"the server did not publish a content view to "
                                 "rebuild"}
                   : context_.content_reload.unavailable_reason;
    }

    const mod_management_context &context_;
    mod_audit_record &audit_;
};

} // namespace

auto mod_management::execute(std::span<const std::string_view> arguments,
                             const mod_management_actor &actor)
    -> mod_management_report {
    const auto started{std::chrono::steady_clock::now()};
    mod_audit_record audit{};
    audit.actor_kind =
        actor.kind == mod_management_actor_kind::console ? "console" : "player";
    audit.actor_name = actor.name;
    audit.actor_uuid = actor.uuid;
    // The configured signer-continuity policy; verify/install overwrite it
    // with what the package itself records (including `unsigned`).
    audit.policy =
        std::string{refrain::mod::to_string(context_.policy.signature_policy)};
    command_runner runner{context_, audit};
    auto report{runner.run(arguments)};
    audit.result = report.outcome;
    audit.detail = report.ok ? std::string{} : report.error;
    audit.duration_ms = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started)
            .count());
    const auto line{format_mod_audit_record(audit)};
    if (context_.audit) {
        context_.audit(audit);
    } else {
        log_audit.info("{}", line);
    }
    return report;
}

auto gamepack_sync_plan::format(std::string_view path) const -> std::string {
    std::string out{fmt::format("gamepack sync: '{}' is out of date:\n", path)};
    for (const auto &change : changes) {
        out += fmt::format("  {}\n", change.line);
    }
    return out;
}

auto installed_versions_from_stores(std::span<const std::string> roots)
    -> result<std::vector<installed_mod_version>> {
    for (const auto &root : roots) {
        if (!refrain::core::io::is_directory(root)) {
            return std::unexpected{make_error(
                error_code::invalid_argument,
                fmt::format("gamepack sync: content store root '{}' is not a "
                            "directory",
                            root))};
        }
    }
    auto catalog{mod::scan_installed(
        roots, mod::discover_options{.require_mods_dir = false})};
    if (!catalog) {
        return std::unexpected{std::move(catalog.error())};
    }
    std::vector<installed_mod_version> installed{};
    installed.reserve(catalog->size());
    for (const auto &candidate : *catalog) {
        installed.push_back({.id = candidate.id, .version = candidate.version});
    }
    std::sort(installed.begin(), installed.end(),
              [](const installed_mod_version &lhs,
                 const installed_mod_version &rhs) { return lhs.id < rhs.id; });
    return installed;
}

auto plan_gamepack_sync(const mod::gamepack_spec &spec,
                        std::span<const installed_mod_version> installed)
    -> result<gamepack_sync_plan> {
    std::map<std::string, std::string> by_id{};
    for (const auto &entry : installed) {
        if (!entry.id.empty() && !entry.version.empty()) {
            by_id[entry.id] = entry.version;
        }
    }

    gamepack_sync_plan plan{};
    // old token -> new token. An exact pin owns its token: a provider
    // requirement spelling the same text must not override it.
    std::map<std::string, std::string> replacements{};
    for (const auto &entry : spec.mods) {
        const auto found{by_id.find(entry.ref.id)};
        if (found == by_id.end() || found->second == entry.ref.version) {
            continue;
        }
        const auto old_text{
            fmt::format("\"{}@{}\"", entry.ref.id, entry.ref.version)};
        const auto new_text{
            fmt::format("\"{}@{}\"", entry.ref.id, found->second)};
        plan.changes.push_back(
            {.line =
                 fmt::format("mod {}: pinned {}, installed {}", entry.ref.id,
                             entry.ref.version, found->second),
             .old_text = old_text,
             .new_text = new_text,
             .section = gamepack_sync_section::mods_list});
        replacements.emplace(old_text, new_text);
    }

    for (const auto &binding : spec.providers) {
        std::vector<mod::mod_requirement> originals{};
        std::vector<std::string> resulting{};
        sync_detail::for_each_requirement(
            binding, [&](const mod::mod_requirement &requirement) {
                originals.push_back(requirement);
                const auto found{by_id.find(requirement.id)};
                if (found == by_id.end()) {
                    resulting.push_back(requirement.text());
                    return;
                }
                auto range{sync_detail::rebase_range(requirement.range,
                                                     found->second)};
                resulting.push_back(
                    range ? fmt::format("{}@{}", requirement.id, *range)
                          : requirement.text());
            });

        // Two distinct ranges can rebase onto one text. parse_gamepack rejects
        // a repeated id@range in one binding, so fail here, before any write.
        std::set<std::string> unique{};
        for (const auto &text : resulting) {
            if (!unique.insert(text).second) {
                return std::unexpected{make_error(
                    error_code::invalid_argument,
                    fmt::format("gamepack sync: provider '{}' would bind '{}' "
                                "more than once after rebasing; refusing to "
                                "change the gamepack",
                                binding.service, text))};
            }
        }

        for (std::size_t index{0}; index < originals.size(); ++index) {
            if (resulting[index] == originals[index].text()) {
                continue;
            }
            const auto old_text{fmt::format("\"{}\"", originals[index].text())};
            const auto new_text{fmt::format("\"{}\"", resulting[index])};
            if (const auto existing{replacements.find(old_text)};
                existing != replacements.end()) {
                if (existing->second != new_text) {
                    return std::unexpected{make_error(
                        error_code::invalid_argument,
                        fmt::format("gamepack sync: token '{}' would become "
                                    "both '{}' and '{}'; refusing to change "
                                    "the gamepack",
                                    old_text, existing->second, new_text))};
                }
                continue; // collapse repeated old -> new edits
            }
            plan.changes.push_back(
                {.line = fmt::format("provider {}: pinned {}, installed {}",
                                     binding.service, originals[index].text(),
                                     resulting[index]),
                 .old_text = old_text,
                 .new_text = new_text,
                 .section = gamepack_sync_section::providers});
            replacements.emplace(old_text, new_text);
        }
    }
    return plan;
}

auto apply_gamepack_sync(std::string_view path, const gamepack_sync_plan &plan)
    -> status {
    auto text{refrain::core::io::read_text(path)};
    if (!text) {
        return std::unexpected{std::move(text.error())};
    }

    // Rewrite line by line so a token in a dependency array or a comment can
    // never be caught by a provider or mods-list replacement.
    std::vector<std::size_t> counts(plan.changes.size(), 0U);
    std::string out{};
    out.reserve(text->size());
    std::optional<gamepack_sync_section> active{};
    std::size_t position{0};
    while (position < text->size()) {
        const auto newline{text->find('\n', position)};
        const auto end{newline == std::string::npos ? text->size() : newline};
        std::string line{text->substr(position, end - position)};
        const auto trimmed{sync_detail::trim(line)};
        if (!trimmed.empty() && trimmed.front() == '#') {
            // Comments are operator prose, never version tokens.
        } else if (!trimmed.empty() && trimmed.front() == '[') {
            const auto close{trimmed.find(']')};
            active =
                close == std::string_view::npos
                    ? std::nullopt
                    : sync_detail::section_of(trimmed.substr(1, close - 1));
        } else if (active) {
            for (std::size_t index{0}; index < plan.changes.size(); ++index) {
                const auto &change{plan.changes[index]};
                if (change.section != *active) {
                    continue;
                }
                std::size_t at{0};
                while ((at = line.find(change.old_text, at)) !=
                       std::string::npos) {
                    line.replace(at, change.old_text.size(), change.new_text);
                    at += change.new_text.size();
                    ++counts[index];
                }
            }
        }
        out += line;
        if (newline == std::string::npos) {
            break;
        }
        out.push_back('\n');
        position = newline + 1;
    }
    for (std::size_t index{0}; index < counts.size(); ++index) {
        if (counts[index] == 0U) {
            return std::unexpected{make_error(
                error_code::not_found,
                fmt::format(
                    "gamepack sync: '{}' has no '{}' in its {} section; "
                    "re-read the gamepack before writing",
                    path, plan.changes[index].old_text,
                    plan.changes[index].section ==
                            gamepack_sync_section::mods_list
                        ? "[mods]"
                        : "[providers]"))};
        }
    }
    return refrain::core::io::atomic_write_text(path, out);
}

auto run_gamepack_sync(const gamepack_sync_options &options) -> int {
    if (options.check && options.write) {
        return report_error(
            make_error(error_code::invalid_argument,
                       "gamepack sync: --check and --write are mutually "
                       "exclusive"));
    }
    if (options.gamepack.empty()) {
        return report_error(
            make_error(error_code::invalid_argument,
                       "gamepack sync: --gamepack <path> is required"));
    }
    auto spec{mod::load_gamepack_spec(options.gamepack, "server")};
    if (!spec) {
        return report_error(spec.error());
    }
    auto installed{installed_versions_from_stores(options.roots)};
    if (!installed) {
        return report_error(installed.error());
    }
    auto plan{plan_gamepack_sync(*spec, *installed)};
    if (!plan) {
        return report_error(plan.error());
    }
    std::optional<std::string> original{};
    bool wrote_gamepack{false};
    if (!plan->empty()) {
        std::cout << plan->format(options.gamepack);
        if (!options.write) {
            return report_error(make_error(
                error_code::version_mismatch,
                fmt::format("gamepack sync: '{}' has out-of-date version pins; "
                            "re-run with --write to apply them",
                            options.gamepack)));
        }
        auto original_text{refrain::core::io::read_text(options.gamepack)};
        if (!original_text) {
            return report_error(original_text.error());
        }
        original = std::move(*original_text);
        if (const auto written{apply_gamepack_sync(options.gamepack, *plan)};
            !written) {
            return report_error(written.error());
        }
        wrote_gamepack = true;
        std::cout << fmt::format("gamepack sync: '{}' updated ({} pin(s))\n",
                                 options.gamepack, plan->changes.size());
    } else {
        std::cout << fmt::format("gamepack sync: '{}' pins are up to date\n",
                                 options.gamepack);
    }

    if (!options.write) {
        return 0;
    }

    // The lock is refreshed through the same path as `mods lock --write` and
    // `refrain-server --update-lock`; sync never implements a second writer.
    mods_lock_options lock_options{};
    lock_options.gamepack = options.gamepack;
    lock_options.roots = options.roots;
    lock_options.image = options.image;
    lock_options.lock = options.lock;
    lock_options.write = true;
    const auto locked{run_mods_lock(lock_options)};
    if (locked != 0 && wrote_gamepack) {
        // The gamepack and the lock must describe one composition. A lock that
        // cannot be refreshed leaves the old pins in place so the pair stays
        // consistent; the lock writer is atomic and has not changed on error.
        (void)refrain::core::io::atomic_write_text(options.gamepack, *original);
    }
    return locked;
}

} // namespace refrain::app
