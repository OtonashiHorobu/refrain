// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <refrain/core/error.hpp>
#include <refrain/data/checksum.hpp>
#include <refrain/data/datapack.hpp>
#include <refrain/data/registry.hpp>
#include <refrain/data/resource_location.hpp>
#include <refrain/data/vfs.hpp>

#include <array>
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
// `tags()`, `packs()`, `registry_inputs()`, `registries()` and the diagnostics
// are safe from multiple threads once `build()` returned. `build()` itself is
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
/// bytes. How a change to one of these files can be applied depends on its
/// class (see `classify_registry_input_path`): a per-entry document becomes a
/// new immutable `registry_generation`, while a registry container keeps its
/// numeric identities frozen in memory and needs a restart.
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

/// Which content registry a registry input path names.
enum class registry_kind : std::uint8_t {
    block = 0,
    item = 1,
    entity_type = 2,
};

inline constexpr std::size_t registry_kind_count = 3;

/// Directory name of a registry kind (`block`, `item`, `entity_type`).
[[nodiscard]] auto registry_kind_name(registry_kind kind) noexcept
    -> std::string_view;

/// How a registry input changes the running server.
enum class registry_input_class : std::uint8_t {
    /// A per-entry registry document (`data/<namespace>/block/...` and the
    /// other registry directories). A view can name the entries such a
    /// document defines, so the content can be applied as a new immutable
    /// generation.
    document = 0,
    /// A registry container (`tables.rvtb`, `protocol_registry.json`). The
    /// runtime freezes the numeric identities it holds when the server starts:
    /// the block/item tables and the wire registry catalog. Replacing one while
    /// the server runs cannot be applied as a generation, so it stays
    /// restart-required.
    container = 1,
};

/// Classify `path` as a registry input, or return null when it is reloadable
/// datapack content.
[[nodiscard]] auto classify_registry_input_path(std::string_view path) noexcept
    -> std::optional<registry_input_class>;

/// Registry kind named by a registry document path, or null for any other
/// path.
[[nodiscard]] auto registry_kind_for_path(std::string_view path) noexcept
    -> std::optional<registry_kind>;

/// One entry of one registry generation.
///
/// `id` is the generation-scoped numeric id, assigned by the canonical
/// `namespace:path` key of the entries in ascending order. The assignment is a
/// pure function of the view content, so two builds of the same content assign
/// the same ids and two different views never share an id space. The id is an
/// identity for consumers of the data layer; chunk generation resolves entries
/// by name, and a stored chunk palette keeps canonical block-state keys rather
/// than numeric ids, so a document digest change that keeps the name set does
/// not change generated chunk content.
struct registry_entry {
    registry_kind kind{registry_kind::block};
    registry_id id{};
    resource_location location{};
    digest32 content{};
    /// Merged virtual path of the document that defines the entry.
    std::string path{};

    [[nodiscard]] friend auto operator==(const registry_entry &,
                                         const registry_entry &)
        -> bool = default;
};

/// Immutable id mapping of the block, item and entity registries one view
/// defines.
///
/// The mapping covers the per-entry registry documents of the view. It is
/// built in content-key order (canonical `namespace:path` ascending, per
/// registry kind) and records the content digest of every defining document,
/// so it is deterministic and detects an edit even when the path is unchanged.
/// A reader holds the generation it started with; a published view never
/// mutates a generation, so a consumer that captured a generation keeps it and
/// new chunks can be bound to the newest one.
///
/// The mapping covers the three registry kinds uniformly. Chunk generation
/// consumes the entry *names* (it resolves block names through the generation's
/// state table; item and entity kinds are carried for consumers that read the
/// data layer), so a document edit that keeps the entry names changes this
/// generation's fingerprint but not generated chunk content. A document that
/// cannot define an entry (a non-JSON file under a registry directory, for
/// example) is not part of a mapping; a reload that changes such a path is
/// refused as restart-required instead of publishing an unchanged mapping.
class registry_generation {
  public:
    registry_generation() = default;

    /// Derive the mapping from the registry inputs of one view. Inputs that
    /// are registry containers are ignored: they are not per-entry documents.
    [[nodiscard]] static auto build(std::span<const registry_input> inputs)
        -> registry_generation;

    [[nodiscard]] auto empty() const noexcept -> bool;

    /// Number of entries of one registry kind.
    [[nodiscard]] auto size(registry_kind kind) const noexcept -> std::size_t;

    /// Total number of entries across the three registries.
    [[nodiscard]] auto entry_count() const noexcept -> std::size_t;

    /// Entries of one kind in ascending id order (which is ascending canonical
    /// content key).
    [[nodiscard]] auto entries(registry_kind kind) const noexcept
        -> std::span<const registry_entry>;

    /// The entry one location names, or null when this generation does not
    /// define it.
    [[nodiscard]] auto find(registry_kind kind,
                            const resource_location &location) const noexcept
        -> const registry_entry *;

    /// BLAKE3 of the canonical encoding of every entry, kind by kind in
    /// content-key order. Two equal mappings have equal fingerprints; a
    /// changed document digest changes it.
    [[nodiscard]] auto fingerprint() const noexcept -> const digest32 &;

    [[nodiscard]] friend auto operator==(const registry_generation &,
                                         const registry_generation &)
        -> bool = default;

  private:
    std::array<std::vector<registry_entry>, registry_kind_count> entries_{};
    digest32 fingerprint_{};
};

/// Which reload policy the registry change of a candidate view needs.
enum class registry_change_kind : std::uint8_t {
    /// No registry input changed.
    none = 0,
    /// Only per-entry registry documents changed: the change can be published
    /// as a new immutable generation.
    documents = 1,
    /// A registry container changed: the numeric identities the runtime froze
    /// at startup cannot be replaced, so the change is restart-required.
    containers = 2,
    /// Both classes changed. The reload is refused as one unit instead of
    /// applying the document half, which would leave the content store and the
    /// frozen containers describing different registries.
    mixed = 3,
};

/// The registry-input difference between the live view and a candidate view.
struct registry_change {
    registry_change_kind kind{registry_change_kind::none};
    /// Paths of registry inputs the candidate adds, sorted.
    std::vector<std::string> added{};
    /// Paths of registry inputs the candidate no longer ships, sorted.
    std::vector<std::string> removed{};
    /// Paths of registry inputs whose content digest changed, sorted.
    std::vector<std::string> changed{};

    [[nodiscard]] auto empty() const noexcept -> bool {
        return kind == registry_change_kind::none;
    }

    /// True when the change can be published as a new registry generation.
    [[nodiscard]] auto hot_appliable() const noexcept -> bool {
        return kind == registry_change_kind::documents;
    }

    /// True when the change needs a server restart.
    [[nodiscard]] auto restart_required() const noexcept -> bool {
        return kind == registry_change_kind::containers ||
               kind == registry_change_kind::mixed;
    }

    /// Every changed path, sorted, regardless of input class.
    [[nodiscard]] auto paths() const -> std::vector<std::string>;

    /// Changed paths that are per-entry registry documents, sorted.
    [[nodiscard]] auto document_paths() const -> std::vector<std::string>;

    /// Changed paths that are registry containers, sorted.
    [[nodiscard]] auto container_paths() const -> std::vector<std::string>;

    /// True when the change adds or removes a registry entry name. Only such a
    /// change can alter what chunk generation resolves; a change that only
    /// edits document digests keeps every name and therefore leaves generated
    /// chunk content unchanged.
    [[nodiscard]] auto entry_names_changed() const noexcept -> bool {
        return !added.empty() || !removed.empty();
    }

    /// One operator-facing sentence naming the added, removed and changed
    /// paths; empty when the change is empty.
    [[nodiscard]] auto description() const -> std::string;
};

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

    /// Immutable block/item/entity id mapping this view defines.
    ///
    /// The generation is a pure function of the view's registry documents: ids
    /// are assigned in content-key order per registry kind and the document
    /// digests are recorded. A view that defines no documents yields an empty
    /// generation with the same behaviour as an absent one.
    [[nodiscard]] auto registries() const noexcept
        -> const registry_generation &;

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
    registry_generation registries_{};
    datapack_stats stats_{};
    kind_counts counts_{};
    digest32 registry_fingerprint_{};
    digest32 fingerprint_{};
};

/// One operator-facing sentence describing a view: layer/file counts and the
/// parsed content counts. Empty views report that they hold no layers.
[[nodiscard]] auto format_content_view_summary(const content_view &view)
    -> std::string;

/// Diff the registry inputs of two views and classify how a reload may apply
/// the difference.
[[nodiscard]] auto classify_registry_change(const content_view &current,
                                            const content_view &candidate)
    -> registry_change;

/// Describe how the registry inputs of two views differ, or return an empty
/// string when the registry fingerprints match.
///
/// The text names the added, removed and changed paths (bounded to a handful
/// each) so a refused reload tells the operator which files need a restart.
/// `classify_registry_change` returns the same difference with the reload
/// policy attached.
[[nodiscard]] auto describe_registry_change(const content_view &current,
                                            const content_view &candidate)
    -> std::string;

} // namespace refrain::data
