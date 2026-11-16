// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <refrain/core/error.hpp>
#include <refrain/data/checksum.hpp>
#include <refrain/data/datapack.hpp>
#include <refrain/data/vfs.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace refrain::data {

// One content view: an immutable, validated snapshot of the layered datapack
// content a running server reads.
//
// A view is built from a list of *re-openable* layers rather than from already
// opened packs, so the same source list can rebuild the equivalent view when
// the pack contents change on disk. `build()` re-opens every layer in priority
// order (later wins, `data::vfs` semantics), freezes the stack, loads and
// validates the datapack documents through `datapack_set`, and records the
// files that name registry data.
//
// The view is move-only because it owns the parsed datapack set. Readers share
// one view through `std::shared_ptr<const content_view>`; the previous view is
// released when the last reader drops it, so a swap at a tick boundary never
// frees data another thread is still reading.
//
// Thread safety: a fully built view is immutable, so `entries()`, `find()`,
// `tags()`, `packs()`, `registry_inputs()` and the diagnostics are safe from
// multiple threads once `build()` returned. `build()` itself is
// self-contained (no process-wide state) and may run on a worker thread next to
// the live view.

/// How one layer of a content view is opened.
///
/// The values are part of the diagnostic surface and are never reused for a
/// different format.
enum class content_source_kind : std::uint8_t {
    directory = 0,   ///< datapack directory tree (development/world datapack)
    rfs = 1,         ///< RFS1 container produced by `data cook`
    composition = 2, ///< one composition of a content-addressed store
};

[[nodiscard]] auto to_string(content_source_kind kind) noexcept
    -> std::string_view;

/// One re-openable layer, in priority order (later = higher priority).
///
/// `path` is the datapack directory, the `.rfs` file, or - for
/// `content_source_kind::composition` - the content store root; `composition`
/// names the resolution index inside that store. `label` is diagnostic only and
/// never participates in path resolution; empty derives a label from the
/// source.
struct content_source {
    content_source_kind kind{content_source_kind::directory};
    std::string path{};
    std::string composition{};
    std::string label{};

    [[nodiscard]] friend auto operator==(const content_source &,
                                         const content_source &)
        -> bool = default;
};

/// Build options for one view.
struct content_view_build_options {
    /// 0 = accept any format declaration; otherwise forwarded to
    /// `datapack_load_options::expected_pack_format`.
    std::uint32_t expected_pack_format{0};
    /// Decode `data/<ns>/structure/**/*.nbt` templates.
    bool parse_structures{true};
};

/// One file whose content names a block, item or entity registry entry, or one
/// of the registry containers the server freezes at startup.
///
/// `path` is the merged virtual path, `content` the BLAKE3 digest of the file
/// bytes. Registry inputs are the only part of a view whose change a running
/// server cannot apply: the block/item/entity registries are frozen in memory
/// when the server starts, and an existing chunk palette keeps the generation
/// it was written with.
struct registry_input {
    std::string path{};
    digest32 content{};

    [[nodiscard]] friend auto operator==(const registry_input &,
                                         const registry_input &)
        -> bool = default;
};

/// True when `path` names registry data rather than reloadable datapack
/// content.
///
/// The rule covers the paths a mod pack uses for registry documents
/// (`data/<namespace>/block/...`, `data/<namespace>/item/...`,
/// `data/<namespace>/entity_type/...`) and the two registry containers the
/// runtime loads from its data directory when a layer ships them at its root
/// (`tables.rvtb`, `protocol_registry.json`).
[[nodiscard]] auto is_registry_input_path(std::string_view path) noexcept
    -> bool;

/// Immutable, validated content view over one layer stack.
class content_view {
  public:
    content_view(const content_view &) = delete;
    auto operator=(const content_view &) -> content_view & = delete;
    content_view(content_view &&) noexcept;
    auto operator=(content_view &&) noexcept -> content_view &;
    ~content_view();

    /// Open, merge, freeze, load and validate every layer.
    ///
    /// Errors name the layer label and the virtual path: a missing or
    /// unreadable layer keeps its code (`not_found`, `parse_error`, ...), and a
    /// semantic validation failure returns `parse_error` with the first errors
    /// followed by the total count. A `composition` layer whose resolution
    /// index or blob table does not exist is not an error: the layer is counted
    /// in `skipped_sources()` and reported through `warnings()`, so a reload
    /// can publish content before (or after) a composition exists.
    ///
    /// An empty source list (or a list whose composition layers are all
    /// absent) is a valid empty view: it has no `datapack_set`, every query
    /// returns nothing and `empty()` is true.
    [[nodiscard]] static auto build(std::span<const content_source> sources,
                                    content_view_build_options options = {})
        -> result<content_view>;

    /// The layer list the view was built from, in priority order.
    [[nodiscard]] auto sources() const noexcept
        -> std::span<const content_source>;

    /// Frozen merged pack stack (all files, not only the modelled content).
    [[nodiscard]] auto packs() const noexcept -> const vfs &;

    /// Parsed datapack content, or null for an empty view.
    [[nodiscard]] auto datapacks() const noexcept -> const datapack_set *;

    [[nodiscard]] auto empty() const noexcept -> bool;

    /// Number of mounted layers (an absent composition layer is not mounted).
    [[nodiscard]] auto layer_count() const noexcept -> std::size_t;

    [[nodiscard]] auto file_count() const noexcept -> std::size_t;

    /// Content counts of the parsed datapack set; zero for an empty view.
    [[nodiscard]] auto counts() const noexcept -> const kind_counts &;

    /// Full load statistics; the default (all-zero) value for an empty view.
    [[nodiscard]] auto stats() const noexcept -> const datapack_stats &;

    /// Non-fatal build diagnostics, in the order they were produced.
    [[nodiscard]] auto warnings() const noexcept
        -> std::span<const std::string>;

    /// Sources skipped because the layer does not exist yet (`composition`
    /// only), in priority order.
    [[nodiscard]] auto skipped_sources() const noexcept
        -> std::span<const content_source>;

    /// Stored content record for (kind, id), or null.
    [[nodiscard]] auto find(content_kind kind,
                            const resource_location &id) const noexcept
        -> const content_entry *;

    /// All stored records of one kind, sorted by id.
    [[nodiscard]] auto entries(content_kind kind) const
        -> std::vector<const content_entry *>;

    /// Merged tag registry, or null for an empty view.
    [[nodiscard]] auto tags() const noexcept -> const tag_set *;

    /// Registry-naming files of the merged view, sorted by path.
    [[nodiscard]] auto registry_inputs() const noexcept
        -> std::span<const registry_input>;

    /// BLAKE3 of the canonical encoding of `registry_inputs()`. Two views with
    /// the same registry fingerprint would leave the running registries
    /// unchanged.
    [[nodiscard]] auto registry_fingerprint() const noexcept
        -> const digest32 &;

    /// BLAKE3 of the canonical encoding of every layer's entry list (path and
    /// size per file). This is a structural identity: it does not follow file
    /// contents, so it is a diagnostic value, not a change detector.
    [[nodiscard]] auto fingerprint() const noexcept -> const digest32 &;

  private:
    content_view();

    vfs packs_{};
    std::optional<datapack_set> datapacks_{};
    std::vector<content_source> sources_{};
    std::vector<content_source> skipped_sources_{};
    std::vector<std::string> warnings_{};
    std::vector<registry_input> registry_inputs_{};
    datapack_stats stats_{};
    kind_counts counts_{};
    digest32 registry_fingerprint_{};
    digest32 fingerprint_{};
};

/// One operator-facing sentence describing a view: layer/file counts and the
/// parsed content counts. Empty views report that they hold no layers.
[[nodiscard]] auto format_content_view_summary(const content_view &view)
    -> std::string;

/// Describe how the registry inputs of two views differ, or return an empty
/// string when the registry fingerprints match.
///
/// The text names the added, removed and changed paths (bounded to a handful
/// each) so a refused reload tells the operator which files need a restart.
[[nodiscard]] auto describe_registry_change(const content_view &current,
                                            const content_view &candidate)
    -> std::string;

} // namespace refrain::data
