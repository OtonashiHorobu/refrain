// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/app/mod_management.hpp>

#include <refrain/core/io.hpp>
#include <refrain/core/log.hpp>
#include <refrain/core/semver.hpp>
#include <refrain/core/version.hpp>
#include <refrain/data/checksum.hpp>
#include <refrain/data/content_store.hpp>
#include <refrain/mod/rfmod.hpp>

#include <fmt/format.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
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
    "reload <id>[@<version>], trust [list], trust pin <id> [confirm], "
    "trust clear <id> [confirm]; destructive commands need 'confirm' and "
    "install, uninstall and trust accept '--dry-run'"};

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
        if (installed.unpinned_confirmed) {
            lines.push_back("a legacy signer was confirmed for this run");
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

} // namespace refrain::app
