// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/data/content_view.hpp>

#include <refrain/data/content_pack.hpp>
#include <refrain/data/path.hpp>

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace refrain::data {
namespace {

/// Diagnostic prefix every build error carries.
[[nodiscard]] auto layer_error(std::string_view label, const error &failure,
                               error_code fallback) -> error {
    // Keep the code of a structured failure (not_found, parse_error, ...) and
    // only prefix the message with the layer identity.
    return make_error(
        failure.code == error_code::unknown ? fallback : failure.code,
        fmt::format("content view: layer '{}': {}", label, failure.message));
}

/// Append one length-prefixed field, so two different field sequences never
/// encode to the same bytes.
void append_field(std::vector<std::byte> &out, std::string_view text) {
    const auto size{static_cast<std::uint32_t>(text.size())};
    for (std::size_t index{0}; index < sizeof(size); ++index) {
        out.push_back(static_cast<std::byte>((size >> (8U * index)) & 0xFFU));
    }
    const auto bytes{std::as_bytes(std::span{text.data(), text.size()})};
    out.insert(out.end(), bytes.begin(), bytes.end());
}

void append_size(std::vector<std::byte> &out, std::uint64_t value) {
    for (std::size_t index{0}; index < sizeof(value); ++index) {
        out.push_back(static_cast<std::byte>((value >> (8U * index)) & 0xFFU));
    }
}

[[nodiscard]] auto digest_layers(const vfs &packs,
                                 std::span<const content_source> sources)
    -> digest32 {
    std::vector<std::byte> combined{};
    // One digest per mounted layer keeps the working buffer bounded by the
    // largest layer instead of the whole stack.
    for (const auto *layer : packs.packs()) {
        std::vector<std::byte> buffer{};
        append_field(buffer, layer->label());
        append_size(buffer, static_cast<std::uint64_t>(layer->format()));
        append_size(buffer, static_cast<std::uint64_t>(layer->file_count()));
        for (const auto &entry : layer->entries()) {
            append_field(buffer, entry.path);
            append_size(buffer, entry.size);
        }
        const auto layer_digest{blake3(buffer)};
        combined.insert(combined.end(), layer_digest.begin(),
                        layer_digest.end());
    }
    if (packs.pack_count() == 0) {
        // Distinguish an empty view from a view whose sources happen to hash to
        // the same bytes; the source list is part of the identity.
        for (const auto &source : sources) {
            append_field(combined, source.path);
            append_field(combined, source.composition);
            append_size(combined, static_cast<std::uint64_t>(source.kind));
        }
    }
    return blake3(combined);
}

[[nodiscard]] auto
digest_registry_inputs(std::span<const registry_input> inputs) -> digest32 {
    std::vector<std::byte> buffer{};
    for (const auto &input : inputs) {
        append_field(buffer, input.path);
        const auto bytes{std::as_bytes(
            std::span{input.content.data(), input.content.size()})};
        buffer.insert(buffer.end(), bytes.begin(), bytes.end());
    }
    return blake3(buffer);
}

[[nodiscard]] auto join_limited(std::span<const std::string> values,
                                std::size_t limit) -> std::string {
    std::string text{};
    const auto shown{std::min(values.size(), limit)};
    for (std::size_t index{0}; index < shown; ++index) {
        if (!text.empty()) {
            text += ", ";
        }
        text += values[index];
    }
    if (values.size() > shown) {
        text += fmt::format(", and {} more", values.size() - shown);
    }
    return text;
}

[[nodiscard]] auto open_source(const content_source &source)
    -> result<std::unique_ptr<pack>> {
    const auto label{source.label.empty() ? source.path : source.label};
    switch (source.kind) {
    case content_source_kind::directory:
        return open_dir_pack(source.path, label);
    case content_source_kind::rfs:
        return open_rfs_pack(source.path, label);
    case content_source_kind::composition:
        return open_content_pack(source.path, source.composition, label);
    }
    return std::unexpected{make_error(
        error_code::invalid_argument,
        fmt::format("content view: '{}' has an unknown layer kind", label))};
}

[[nodiscard]] auto source_label(const content_source &source) -> std::string {
    if (!source.label.empty()) {
        return source.label;
    }
    if (source.kind == content_source_kind::composition) {
        return fmt::format("{}:{}", source.path, source.composition);
    }
    return source.path;
}

} // namespace

auto to_string(content_source_kind kind) noexcept -> std::string_view {
    switch (kind) {
    case content_source_kind::directory:
        return "directory";
    case content_source_kind::rfs:
        return "rfs";
    case content_source_kind::composition:
        return "composition";
    }
    return "unknown";
}

auto is_registry_input_path(std::string_view path) noexcept -> bool {
    constexpr std::string_view data_prefix{"data/"};
    if (path == "tables.rvtb" || path == "protocol_registry.json") {
        return true;
    }
    if (!path.starts_with(data_prefix)) {
        return false;
    }
    const auto rest{path.substr(data_prefix.size())};
    const auto namespace_end{rest.find('/')};
    if (namespace_end == std::string_view::npos) {
        return false;
    }
    const auto kind_start{namespace_end + 1U};
    const auto kind_end{rest.find('/', kind_start)};
    if (kind_end == std::string_view::npos || kind_end + 1U >= rest.size()) {
        return false;
    }
    const auto kind{rest.substr(kind_start, kind_end - kind_start)};
    return kind == "block" || kind == "item" || kind == "entity_type";
}

content_view::content_view() = default;

content_view::content_view(content_view &&) noexcept = default;

auto content_view::operator=(content_view &&) noexcept
    -> content_view & = default;

content_view::~content_view() = default;

auto content_view::build(std::span<const content_source> sources,
                         content_view_build_options options)
    -> result<content_view> {
    content_view view{};
    view.sources_.assign(sources.begin(), sources.end());
    for (const auto &source : sources) {
        auto opened{open_source(source)};
        if (!opened) {
            if (source.kind == content_source_kind::composition &&
                opened.error().code == error_code::not_found) {
                view.skipped_sources_.push_back(source);
                view.warnings_.push_back(fmt::format(
                    "content layer '{}' is not published, so the view does not "
                    "mount it",
                    source_label(source)));
                continue;
            }
            return std::unexpected{layer_error(source_label(source),
                                               opened.error(),
                                               error_code::invalid_argument)};
        }
        if ((*opened)->file_count() == 0) {
            // `vfs::mount` rejects empty packs; name the layer instead of
            // surfacing the generic mount error.
            return std::unexpected{make_error(
                error_code::invalid_argument,
                fmt::format("content view: layer '{}' is empty, so it cannot "
                            "be mounted",
                            source_label(source)))};
        }
        if (auto mounted{view.packs_.mount(std::move(*opened))}; !mounted) {
            return std::unexpected{layer_error(source_label(source),
                                               mounted.error(),
                                               error_code::invalid_argument)};
        }
    }
    view.packs_.freeze();
    view.fingerprint_ = digest_layers(view.packs_, view.sources_);
    if (view.packs_.pack_count() == 0) {
        view.registry_fingerprint_ =
            digest_registry_inputs(view.registry_inputs_);
        return view;
    }
    auto loaded{datapack_set::load(
        view.packs_, datapack_load_options{
                         .expected_pack_format = options.expected_pack_format,
                         .parse_structures = options.parse_structures})};
    if (!loaded) {
        return std::unexpected{std::move(loaded.error())};
    }
    const auto validation{loaded->validate()};
    if (!validation.ok()) {
        std::string detail{};
        constexpr std::size_t max_named{5};
        const auto named{std::min(validation.errors.size(), max_named)};
        for (std::size_t index{0}; index < named; ++index) {
            if (index != 0) {
                detail += "; ";
            }
            detail += validation.errors[index];
        }
        if (validation.errors.size() > named) {
            detail +=
                fmt::format("; {} more", validation.errors.size() - named);
        }
        return std::unexpected{
            make_error(error_code::parse_error,
                       fmt::format("content view: {} validation errors: {}",
                                   validation.errors.size(), detail))};
    }
    view.warnings_.insert(view.warnings_.end(), validation.warnings.begin(),
                          validation.warnings.end());
    view.counts_ = loaded->counts();
    view.stats_ = loaded->stats();
    view.datapacks_.emplace(std::move(*loaded));
    for (const auto &entry : view.packs_.list()) {
        if (!is_registry_input_path(entry.path)) {
            continue;
        }
        auto bytes{view.packs_.read(entry.path)};
        if (!bytes) {
            return std::unexpected{
                layer_error(entry.path, bytes.error(), error_code::io_error)};
        }
        view.registry_inputs_.push_back(
            registry_input{.path = entry.path, .content = blake3(*bytes)});
    }
    view.registry_fingerprint_ = digest_registry_inputs(view.registry_inputs_);
    return view;
}

auto content_view::sources() const noexcept -> std::span<const content_source> {
    return sources_;
}

auto content_view::packs() const noexcept -> const vfs & { return packs_; }

auto content_view::datapacks() const noexcept -> const datapack_set * {
    return datapacks_ ? &*datapacks_ : nullptr;
}

auto content_view::empty() const noexcept -> bool {
    return packs_.pack_count() == 0;
}

auto content_view::layer_count() const noexcept -> std::size_t {
    return packs_.pack_count();
}

auto content_view::file_count() const noexcept -> std::size_t {
    return packs_.file_count();
}

auto content_view::counts() const noexcept -> const kind_counts & {
    return counts_;
}

auto content_view::stats() const noexcept -> const datapack_stats & {
    return stats_;
}

auto content_view::warnings() const noexcept -> std::span<const std::string> {
    return warnings_;
}

auto content_view::skipped_sources() const noexcept
    -> std::span<const content_source> {
    return skipped_sources_;
}

auto content_view::find(content_kind kind,
                        const resource_location &id) const noexcept
    -> const content_entry * {
    return datapacks_ ? datapacks_->find(kind, id) : nullptr;
}

auto content_view::entries(content_kind kind) const
    -> std::vector<const content_entry *> {
    return datapacks_ ? datapacks_->entries(kind)
                      : std::vector<const content_entry *>{};
}

auto content_view::tags() const noexcept -> const tag_set * {
    return datapacks_ ? &datapacks_->tags() : nullptr;
}

auto content_view::registry_inputs() const noexcept
    -> std::span<const registry_input> {
    return registry_inputs_;
}

auto content_view::registry_fingerprint() const noexcept -> const digest32 & {
    return registry_fingerprint_;
}

auto content_view::fingerprint() const noexcept -> const digest32 & {
    return fingerprint_;
}

auto format_content_view_summary(const content_view &view) -> std::string {
    if (view.empty()) {
        return "no content layers are mounted";
    }
    const auto &counts{view.counts()};
    return fmt::format(
        "{} content layers, {} files, {} recipes, {} loot tables, {} "
        "advancements, {} worldgen documents, {} structures and {} tags",
        view.layer_count(), view.file_count(), counts.recipe, counts.loot_table,
        counts.advancement, counts.worldgen, counts.structure, counts.tags);
}

auto describe_registry_change(const content_view &current,
                              const content_view &candidate) -> std::string {
    if (current.registry_fingerprint() == candidate.registry_fingerprint()) {
        return {};
    }
    const auto current_inputs{current.registry_inputs()};
    const auto candidate_inputs{candidate.registry_inputs()};
    std::map<std::string, digest32> live{};
    for (const auto &input : current_inputs) {
        live.emplace(input.path, input.content);
    }
    std::vector<std::string> added{};
    std::vector<std::string> changed{};
    for (const auto &input : candidate_inputs) {
        const auto found{live.find(input.path)};
        if (found == live.end()) {
            added.push_back(input.path);
        } else if (found->second != input.content) {
            changed.push_back(input.path);
        }
    }
    std::map<std::string, digest32> next{};
    for (const auto &input : candidate_inputs) {
        next.emplace(input.path, input.content);
    }
    std::vector<std::string> removed{};
    for (const auto &input : current_inputs) {
        if (!next.contains(input.path)) {
            removed.push_back(input.path);
        }
    }
    constexpr std::size_t max_named{3};
    std::string text{};
    const auto append = [&text](std::string_view kind,
                                const std::vector<std::string> &paths) {
        if (paths.empty()) {
            return;
        }
        if (!text.empty()) {
            text += "; ";
        }
        text += kind;
        text += ' ';
        text += join_limited(paths, max_named);
    };
    append("added", added);
    append("removed", removed);
    append("changed", changed);
    return text;
}

} // namespace refrain::data
