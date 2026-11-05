// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <refrain/core/error.hpp>
#include <refrain/data/checksum.hpp>
#include <refrain/data/content_store.hpp>
#include <refrain/data/signature.hpp>
#include <refrain/mod/manifest.hpp>
#include <refrain/mod/origin.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace refrain::mod {

// `.rfmod` — deterministic mod package format.
//
// Container layer: an `.rfmod` is a single zstd stream wrapping a tar stream.
// The write side (`build_rfmod`/`pack_mod_directory`) emits a *minimal, strict*
// ustar tar:
//  - regular-file entries only, sorted by path bytes; directories are not
//    written;
//  - mode 0644, uid/gid 0, mtime 0, empty uname/gname, ustar magic/version;
//  - names must fit the ustar name/prefix fields (the writer emits no PAX/GNU
//    extensions);
//  - two zero blocks terminate the archive, nothing follows.
// The small writer is deliberate: the deterministic packaging format is
// defined by this project, so byte-for-byte reproducibility wins over reusing
// libarchive's writer (whose metadata and member-ordering choices are not part
// of our contract).
//
// Two builds of the same file set are therefore byte-identical regardless of
// input order, and the archive BLAKE3 is a stable content identity.
//
// The read side (`rfmod_reader`) parses the already-decompressed
// stream with libarchive's tar reader: ustar, PAX extended headers (paths
// >100 bytes) and GNU long-name ('L') entries are accepted. zstd
// decompression stays in this project's bounded streaming stage; libarchive is
// opened in-memory with tar format support only (no filters). Directory
// entries are skipped (they carry no payload and are optional);
// symlink/hardlink/device/FIFO/socket and sparse entries are rejected (the
// packer never emits sparse members, and their logical size can vastly exceed
// their stored bytes). Member paths must be safe relative paths (no
// absolute/`..`/backslash/control bytes) that are valid UTF-8 (RFC 3629), and
// unique. Declared sizes/counts and the sum of member payloads are bounded by
// `rfmod_read_options`; independently, each member's declared size must fit in
// the bytes left in the tar stream (which also catches sparse forms that
// report no stored extent). Sparse rejection additionally keeps the member
// bytes within the decompressed tar size, so peak reader memory is the tar
// plus its payloads (≈2× `max_archive_bytes`). Two zero blocks must terminate
// the stream immediately after the last member (trailing zero padding is
// fine), so a truncated archive whose member payload happens to end in zero
// bytes is not silently accepted.
//
// Package layer: `pack_mod_directory` embeds a package
// manifest. Besides the source `[mod]`/`[meta]`/`[requires]`/... tables, the
// shipped `manifest.toml` carries:
//
//   [files]        every archive member except manifest.toml, sorted by path
//                  *bytes*: `"<path>" = { size = <uint>, blake3 = "<hex64>",
//                  mode = 0o644 }` (manifest.toml cannot hash itself; the
//                  signature covers it instead);
//   [packaging]    tool / tool_version / zstd_level / deterministic = true /
//                  format_version = 1 (deterministic build record);
//   [signature]    scheme = "ed25519", key_id, sig (only for signed packages;
//                  always the last table of the file).
//
// `[files]` paths are safe relative UTF-8 paths: non-empty, no leading '/', no
// empty/'.'/'..' segments, no backslash, no control bytes/NUL, no '"', and
// well-formed UTF-8 (RFC 3629). Duplicate paths are impossible in TOML (the
// parser rejects them).
//
// Canonical signed bytes
// ----------------------
// The signature covers the raw manifest bytes *before* the line that carries
// the `[signature]` table header, i.e. `text[0, header_line_start)` where
// `text` is the exact `manifest.toml` found in the archive. Nothing is
// re-serialized, normalized or re-ordered: the file's own bytes are the
// message. A re-indented/re-formatted manifest therefore verifies only if the
// re-formatting happened before signing.
//
// `pack_mod_directory` builds that prefix deterministically:
//
//   <source manifest with trailing whitespace/newlines stripped>
//   \n\n[files]\n"<path>" = { size = N, blake3 = "...", mode = 0o644 }\n...
//   [packaging]\ntool = "refrain-tools"\n...
//
// and, when signing, appends one blank line plus the `[signature]` table. The
// blank line is part of the signed prefix because it precedes the header line:
// the packer signs exactly `text[0, header_line_start)` too, so producer and
// verifier cannot disagree.
//
// `[signature]` must be the *only* signature table and the *last* table in the
// file; after its header line only its own keys are allowed. A second
// `[signature]` header, or any other table after it, is rejected — otherwise
// one file could carry two contradictory signatures. `pack_mod_directory`
// refuses a source manifest that already contains these tables (that would be
// double-packaging an already packed/signed package).
//
// Trust policy: first-party/release packages are expected to be
// signed; third-party mods may stay unsigned. `verify_rfmod` reports
// `signed_package`/`trusted` separately, and `require_signature` turns an
// unsigned or untrusted package into an error.
//
// `build_rfmod`/`write_rfmod` stay *pure containers*: they never inject
// `[files]`/`[packaging]`/`[signature]`, so their output is not an installable
// package — `verify_rfmod`/`install_rfmod` reject it with `parse_error`
// (missing `[files]`). Use `pack_mod_directory` for packages.

/// `.rfmod` container format version (`[packaging].format_version`, and the
/// optional `[mod].format_version`). Integer, not SemVer.
inline constexpr std::uint32_t rfmod_format_version{1};

/// Root manifest file name inside every `.rfmod`.
inline constexpr std::string_view rfmod_manifest_path{"manifest.toml"};
inline constexpr std::string_view rfmod_extension{".rfmod"};

/// Tar block size (bytes).
inline constexpr std::size_t rfmod_tar_block_size{512};

/// Reader safety limits (hostile archive defence).
inline constexpr std::size_t rfmod_default_max_archive_bytes{
    512ULL * 1024ULL * 1024ULL}; ///< decompressed tar bytes
/// Compressed container bytes accepted by `rfmod_reader::open_file`. Checked
/// with `core::io::file_size` *before* the file is read, so a sparse or
/// oversized file cannot force the reader to allocate its full size first.
inline constexpr std::size_t rfmod_default_max_container_bytes{
    512ULL * 1024ULL * 1024ULL}; ///< compressed container bytes
inline constexpr std::size_t rfmod_default_max_file_bytes{64ULL * 1024ULL *
                                                          1024ULL};
inline constexpr std::size_t rfmod_default_max_files{1ULL << 16};

/// Deterministic zstd level used for packaging by default.
inline constexpr std::int32_t rfmod_default_zstd_level{3};

/// One file to be written into an `.rfmod`.
struct rfmod_input {
    std::string path{};              ///< '/'-separated, relative, safe
    std::span<const std::byte> data; ///< borrowed for the build call
};

/// One `[files]` record of a package manifest.
struct rfmod_file_record {
    std::string path{};
    std::uint64_t size{0};
    data::digest32 blake3{};
    std::uint32_t mode{0644};
};

/// Parsed `[signature]` table of a signed package.
struct rfmod_signature_info {
    std::string scheme{};  ///< "ed25519" (checked by `verify_rfmod`)
    std::string key_id{};  ///< non-empty key identifier (key rotation/audit)
    std::string sig_hex{}; ///< 128 lowercase hex characters
};

/// A public key the caller trusts, addressed by its manifest `key_id`.
///
/// Trust is pinned by the *caller* (config/CLI); this type only binds a key id
/// to key bytes. An unknown `key_id` is never trusted implicitly.
struct rfmod_trusted_key {
    std::string key_id{};
    data::ed25519_public_key public_key{};
};

/// Signing material for `pack_mod_directory` (64-byte libsodium layout; derive
/// it from a seed with `data::keypair_from_seed`).
struct rfmod_signing_options {
    data::ed25519_secret_key secret_key{};
    std::string key_id{};
};

/// Write options for `build_rfmod` / `write_rfmod` / `pack_mod_directory`.
struct rfmod_write_options {
    /// zstd compression level (clamped to 1..ZSTD_maxCLevel).
    std::int32_t zstd_level{rfmod_default_zstd_level};
    /// Refuse archives that would exceed the reader's default file limits so
    /// every package this code produces is readable with default options.
    std::size_t max_files{rfmod_default_max_files};
    std::size_t max_file_bytes{rfmod_default_max_file_bytes};
    /// Sign the package manifest with Ed25519. `nullopt` produces an unsigned
    /// package that still carries `[files]`/`[packaging]` (unsigned third-party
    /// mods are allowed). `pack_mod_directory` only; the pure
    /// container builders ignore it.
    std::optional<rfmod_signing_options> signing{};
};

/// Reader limits/options.
struct rfmod_read_options {
    /// Maximum decompressed tar size (zip-bomb guard). Also bounds the summed
    /// member payloads: peak reader memory stays below tar + members.
    std::size_t max_archive_bytes{rfmod_default_max_archive_bytes};
    /// Maximum compressed container size accepted by `open_file`. The size is
    /// checked with `core::io::file_size` before the file is read (and again
    /// after, in case it grew meanwhile), so a sparse or oversized file cannot
    /// make the reader allocate its full size first (DoS).
    std::size_t max_container_bytes{rfmod_default_max_container_bytes};
    /// Maximum size of one member.
    std::size_t max_file_bytes{rfmod_default_max_file_bytes};
    /// Maximum number of members.
    std::size_t max_files{rfmod_default_max_files};
};

/// One member of an opened archive (paths are validated + unique).
struct rfmod_entry {
    std::string path{};
    std::uint64_t size{0};
};

/// Build an `.rfmod` in memory (deterministic: sorted unique paths, fixed tar
/// metadata, fixed zstd level). Fails with `invalid_argument` for unsafe/
/// duplicate/oversized paths or inputs exceeding the configured limits.
///
/// Pure container builder: it does not add `[files]`/`[packaging]`/
/// `[signature]` and ignores `options.signing`; the result is not an
/// installable package.
[[nodiscard]] auto build_rfmod(std::span<const rfmod_input> inputs,
                               const rfmod_write_options &options = {})
    -> result<std::vector<std::byte>>;

/// `build_rfmod` + `core::io::atomic_write_file`. Returns the archive BLAKE3.
[[nodiscard]] auto write_rfmod(std::string_view path,
                               std::span<const rfmod_input> inputs,
                               const rfmod_write_options &options = {})
    -> result<data::digest32>;

/// Result of `pack_mod_directory`.
struct packed_rfmod {
    std::string archive_path{};
    data::digest32 archive_digest{};
    std::size_t file_count{0};
    std::uint64_t total_bytes{0};
};

/// Package a `mods/<id>/` directory tree into `output_path` (atomic write).
///
/// Included: `manifest.toml` (required, parsed with the mod manifest
/// parser), `data/**`, `assets/**`, `scripts/**`, root `LICENSE`/`NOTICE`.
/// Excluded: `src/**` (internal code never ships), dotfiles/dot
/// directories, any other root entry (READMEs, build output, ...).
///
/// The shipped manifest is the source manifest plus the package tables
/// (`[files]` records for every shipped file except manifest.toml, then
/// `[packaging]`, then the optional `[signature]`). A source manifest that
/// already contains package tables is rejected with `invalid_argument`
/// (double packaging). `mode` is recorded as 0644 because the deterministic
/// tar writer emits 0644 for every member. Packaging the same tree with the
/// same options is byte-identical, signed or not (Ed25519 is deterministic).
[[nodiscard]] auto pack_mod_directory(std::string_view mod_directory,
                                      std::string_view output_path,
                                      const rfmod_write_options &options = {})
    -> result<packed_rfmod>;

/// Read-only `.rfmod` archive handle. Move-only; all errors are values.
class rfmod_reader {
  public:
    rfmod_reader(rfmod_reader &&other) noexcept;
    auto operator=(rfmod_reader &&other) noexcept -> rfmod_reader &;
    ~rfmod_reader();

    rfmod_reader(const rfmod_reader &) = delete;
    auto operator=(const rfmod_reader &) -> rfmod_reader & = delete;

    /// Open from memory. The compressed bytes are hashed; the decompressed tar
    /// is parsed by libarchive (ustar/PAX/GNU) and validated with the same
    /// path/type/size rules the container writer enforces.
    [[nodiscard]] static auto open(std::span<const std::byte> archive,
                                   rfmod_read_options options = {})
        -> result<rfmod_reader>;

    /// Bounded read (`file_size` check against `max_container_bytes`) +
    /// `core::io::read_file` + `open`.
    [[nodiscard]] static auto open_file(std::string_view path,
                                        rfmod_read_options options = {})
        -> result<rfmod_reader>;

    /// Members in archive order (sorted by path for packages we produce).
    [[nodiscard]] auto entries() const noexcept
        -> const std::vector<rfmod_entry> &;

    [[nodiscard]] auto find(std::string_view path) const noexcept
        -> const rfmod_entry *;

    /// Copy one member's bytes.
    [[nodiscard]] auto read(std::string_view path) const
        -> result<std::vector<std::byte>>;

    /// BLAKE3 of the compressed archive bytes (stable package identity).
    [[nodiscard]] auto archive_digest() const noexcept
        -> const data::digest32 &;

    /// Size of the decompressed tar stream (bounded by the read options).
    [[nodiscard]] auto decompressed_size() const noexcept -> std::size_t;

    [[nodiscard]] auto file_count() const noexcept -> std::size_t;

  private:
    rfmod_reader();

    struct impl;
    std::unique_ptr<impl> impl_;
};

/// Verification policy. The archive must already be open (limits are applied
/// by `rfmod_reader::open`); `read` is carried for helpers/CLI code that want
/// to hand the same option bundle to `open_file`.
struct rfmod_verify_options {
    /// Keys the caller trusts, matched by manifest `key_id`. Empty means "no
    /// keys configured": an unsigned package is accepted unless
    /// `require_signature`, and a *signed* package with no matching key is
    /// accepted with `trusted = false`.
    std::vector<rfmod_trusted_key> trusted_keys{};
    /// Reject unsigned packages (and signed packages with no configured key).
    bool require_signature{false};
    rfmod_read_options read{};
};

/// Result of a successful `verify_rfmod`.
struct rfmod_verification {
    mod::mod_manifest manifest{};
    std::vector<rfmod_file_record> files{}; ///< sorted by manifest key order
    std::optional<rfmod_signature_info> signature{};
    data::digest32 archive_digest{};  ///< BLAKE3 of the compressed bytes
    data::digest32 manifest_digest{}; ///< BLAKE3 of the full manifest.toml
                                      ///< (including the signature block)
    bool signed_package{false};
    bool trusted{false};
    std::string key_id{};
};

/// Verify a package completely before anything is installed.
///
/// Steps, in order (any failure returns an error, never a partial success):
///  1. read `manifest.toml` (`not_found` when absent) and split the raw bytes
///     at the `[signature]` header line (see "Canonical signed bytes");
///  2. parse the whole manifest (manifest parser with package tables enabled)
///     and validate `[files]`, `[packaging]` and `[signature]` strictly;
///     `format_version` must equal `rfmod_format_version`
///     (`version_mismatch`);
///  3. coverage both ways: every archive member except `manifest.toml` must
///     have a record whose size and BLAKE3 match, and every record must exist
///     in the archive (`parse_error` otherwise);
///  4. signature policy:
///     - unsigned: `require_signature` → `not_found`; else success with
///       `signed_package = false`;
///     - signed, scheme != "ed25519" → `invalid_argument`;
///     - matching trusted key: verification failure → `parse_error`, success
///       → `trusted = true`;
///     - no matching key: non-empty `trusted_keys` → `not_found`, else
///       `require_signature` → `invalid_argument`, else accepted with
///       `trusted = false` (signature info retained for audit).
[[nodiscard]] auto verify_rfmod(const rfmod_reader &reader,
                                const rfmod_verify_options &options = {})
    -> result<rfmod_verification>;

/// Options for `install_rfmod`.
struct rfmod_install_options {
    /// Require a signature from one of `trusted_keys` (a signed package with
    /// an unknown key is rejected too).
    bool require_signature{false};
    /// Trust anchors used by the package verification step.
    std::vector<rfmod_trusted_key> trusted_keys{};
    /// Publish the per-mod install receipt (`<store>/mods/<id>/<version>.json`)
    /// after the batch commit and blob-table publish. `false` keeps the CAS
    /// writes but records nothing (tools/tests that only want the blobs).
    bool write_receipt{true};
    /// Take `<store>/content/.lock` for the whole install (default). The
    /// startup bootstrap already holds that lock and must pass `false`, or the
    /// install would wait on its own lock.
    bool lock_store{true};
    /// Host-observed origin recorded in the install receipt.
    /// Manifests cannot set it; callers pass the tier of the root the package
    /// came from. Receipts written before this field default to `user` when
    /// read.
    mod_origin origin{mod_origin::user};
};

/// One installed member (path as stored, CAS digest, size and recorded mode).
struct installed_file {
    std::string path{};
    data::digest32 digest{};
    std::uint64_t size{0};
    std::uint32_t mode{0644};
};

/// Result of installing an archive into the content store.
struct rfmod_install_result {
    mod::mod_manifest manifest{};
    data::digest32 archive_digest{};
    data::digest32 manifest_digest{};
    std::vector<installed_file> files{}; ///< in archive order
    std::size_t stored_blobs{0};         ///< members ingested by this install
    /// Distinct blobs newly written by this install's batch.
    std::size_t new_blobs{0};
    bool signed_package{false};
    bool trusted{false};
    std::string key_id{};
    mod_origin origin{mod_origin::user};
    /// `<store>/mods/<id>/<version>.json` when the receipt was written; empty
    /// when `write_receipt` is `false` (or on failure, which is an error).
    std::string receipt_path{};
};

/// One installed member recorded in a `refrain.mod-receipt/1` receipt.
struct receipt_file {
    std::string path{};
    std::uint64_t size{0};
    data::digest32 blake3{};
};

/// Validated `<store>/mods/<id>/<version>.json` install receipt.
///
/// The receipt is also a content GC root: its `files[].blake3` digests are
/// referenced by the installed mod even before any resolution index exists, so
/// `content gc` must keep those blobs/segments. This is why the parser is part
/// of the public mod API instead of a CLI-local helper.
struct mod_receipt {
    std::string id{};
    std::string version{};
    std::uint32_t abi{0};
    data::digest32 archive_blake3{};
    data::digest32 manifest_blake3{};
    bool signed_package{false};
    bool trusted{false};
    std::string key_id{};
    /// Host-observed install tier (missing field in an older receipt = user).
    mod_origin origin{mod_origin::user};
    std::vector<receipt_file> files{};
};

/// Parse and strictly validate one install receipt.
///
/// Checks the JSON object, `schema == "refrain.mod-receipt/1"`, every required
/// top-level field/type, SemVer `version`, integer `abi`, 64-hex
/// `archive_blake3`/`manifest_blake3`, and every `files[]` entry's safe
/// relative UTF-8 `path`, non-negative `size`, 64-hex `blake3` and (when
/// present) 0..0777 `mode`. A corrupt/foreign receipt is `parse_error` (the
/// message names `path`), never silently skipped.
[[nodiscard]] auto read_receipt(std::string_view path) -> result<mod_receipt>;

/// Sorted `<store>/mods/<id>/<version>.json` receipt paths.
///
/// A missing `<store>/mods` directory is an empty list; missing subdirectories
/// and non-`.json`/non-regular entries are skipped. Read each path with
/// `read_receipt` before using it as a GC root.
[[nodiscard]] auto list_receipt_paths(std::string_view store_root)
    -> result<std::vector<std::string>>;

/// Fully verify the package (`verify_rfmod`), then ingest every member
/// (including `manifest.toml`) through one content-store batch and parse the
/// manifest.
///
/// One install is one `data::batch` / one `commit()`:
/// members of at most 64 KiB (including `manifest.toml` and zero-byte files)
/// are appended to the immutable `content/segments/*.rfseg` files; larger
/// members keep their own packed `content/blobs/<hh>/<digest>` file. The
/// existing `<store>/content/blobs.idx` is parsed before the first write (a
/// damaged or foreign-version table fails the install), and its records are
/// both the batch's dedupe set and the metadata carried into the republished
/// table. An install never rewrites an existing record; the batch's new blobs
/// are added with `kind = content` and `refcount = 0` until a resolution index
/// or another root claims them. Publishing a changed table changes its BLAKE3
/// prefix and can shift `blob_index` positions, so every existing
/// `content/resolutions/*.idx` is re-encoded against the merged table (the same
/// transaction `content build` performs). The table and those indexes are
/// built in memory, published atomically only after `commit()`, and the receipt
/// only after that. A damaged existing table or index fails before the first
/// write.
///
/// Verification happens *before* the first CAS write, so a rejected package
/// leaves no blob behind. Each member's BLAKE3 must equal its `[files]` record
/// (the manifest member is checked against `manifest_digest`); a mismatch is
/// `parse_error`. Content addressing is append-only across process crashes (a
/// crash mid-install can leave already-written blobs/segments; harmless, and a
/// later GC collects them).
///
/// Receipt: once every member is in the CAS, its digest has been
/// verified and the batch committed, a deterministic JSON record is atomically
/// written to `<store>/mods/<id>/<version>.json`:
///
///   `schema` = "refrain.mod-receipt/1", `id`, `version`, `abi`,
///   `archive_blake3`, `manifest_blake3`, `signed`, `trusted`, `key_id`,
///   `origin` (`builtin`|`first_party`|`user`), and
///   `files` (one `path`/`size`/`blake3`/`mode` object per installed member,
///   sorted by path bytes).
///
/// `id`/`version` are re-checked as safe single path segments (non-empty, not
/// exactly "."/"..", no '/'/'\\'/control bytes; ordinary dots inside a name,
/// e.g. the valid mod id `a..b`, are allowed) *before the first CAS write*, so
/// an unsafe segment fails without leaving a blob behind. The receipt carries
/// no timestamps or host paths, so reinstalling the same package yields
/// byte-identical bytes and `refrain-tools mods installed` can list it. A table
/// or receipt write failure is `io_error`; blobs and sealed segments already
/// written by this call are kept (append-only CAS semantics — no rollback).
///
/// The receipt is also a content GC root: `content gc` treats every
/// `files[].blake3` as referenced, so an installed mod's blobs/segments survive
/// even though install writes no resolution index of its own. `read_receipt`/
/// `list_receipt_paths` are the library API for that root set (and for
/// `mods installed`). `write_receipt = false` skips that root record: the blob
/// table is still published for dedupe, but GC may later collect the blobs as
/// unreferenced.
[[nodiscard]] auto install_rfmod(const rfmod_reader &reader,
                                 data::content_store &store,
                                 const rfmod_install_options &options = {})
    -> result<rfmod_install_result>;

} // namespace refrain::mod
