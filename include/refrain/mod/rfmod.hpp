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

/// Lowercase hex BLAKE3 fingerprint of a public key (64 characters).
///
/// The fingerprint identifies the *key material* a `key_id` refers to, so a
/// receipt or a signer pin can tell a renamed/reused `key_id` from the key that
/// signed the previous version. Signer continuity compares this value, not the
/// (caller-chosen) key id alone.
[[nodiscard]] auto public_key_fingerprint(const data::ed25519_public_key &key)
    -> std::string;

/// Signature continuity policy for updates of an already installed mod id
/// ("Android" update semantics).
enum class rfmod_signature_policy : std::uint8_t {
    /// Updates must keep the pinned signer. A different signer is rejected
    /// unless an explicit key rotation allowlist entry names the transition.
    strict = 0,
    /// Any trusted signature is accepted for an update. Intended for
    /// self-hosted/development stores only; the install pins the newest
    /// signer either way.
    trusted_any = 1,
};

[[nodiscard]] auto to_string(rfmod_signature_policy policy) noexcept
    -> std::string_view;
[[nodiscard]] auto parse_rfmod_signature_policy(std::string_view text) noexcept
    -> std::optional<rfmod_signature_policy>;

/// One explicitly allowed signer transition for a mod id.
///
/// A rotation entry is the only way a `strict` update may change its signing
/// key; it is a trust-anchor-level decision and belongs in server
/// configuration, not in the package.
struct rfmod_key_rotation {
    std::string id{};
    std::string old_key_id{};
    std::string new_key_id{};
};

/// Pinned signer identity of one mod id
/// (`<store>/mods/pins/<id>.json`, schema `refrain.mod-signer-pin/1`).
///
/// The pin is written by the first successful signed install and checked on
/// every later update; `uninstall` clears it when the last installed version is
/// removed unless the caller keeps it. A receipt v2 also carries the signer, so
/// a missing pin file is derived from the highest installed receipt; a v1
/// receipt has no fingerprint and counts as *unpinned* (legacy) until an
/// administrator confirms the signer.
struct mod_signer_pin {
    std::string id{};
    std::string key_id{};
    /// Hex64 fingerprint of the pinned key. Empty when the package was signed
    /// by a key with no configured trust anchor (only the key id is known).
    std::string public_key_fingerprint{};
    rfmod_signature_policy policy{rfmod_signature_policy::strict};
    /// RFC 3339 UTC timestamp, empty for pins migrated from an older store.
    std::string pinned_at{};
    /// `install`, `rotation`, `admin` or `uninstall` (audit only).
    std::string source{};
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
    /// Hex64 fingerprint of the verifying key when it is a configured trust
    /// anchor (`trusted == true`); empty for an unsigned package or a signed
    /// package whose key bytes are not configured.
    std::string public_key_fingerprint{};
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
///
/// Install is the only way an external mod enters the runtime: the archive is
/// verified, unpacked into a staging directory, committed to the CAS, a receipt
/// is published and only then is the mod loadable. The options below add the
/// update semantics that a plain "verify and unpack" install lacks:
///
///  * signer continuity — the first signed install pins the signer for the mod
///    id; later versions must carry the same `key_id`/fingerprint, or an
///    explicit `rotations` entry must allow the transition;
///  * signed state — a signed mod cannot be updated by an unsigned package
///    (an unsigned mod may be upgraded to a signed one, which pins it);
///  * version monotonicity — a version lower than the highest installed one is
///    rejected unless `allow_downgrade`;
///  * content identity — the same `(id, version)` with a different archive
///    digest is rejected unless `replace_same_version`.
///
/// Every override is audited (`rfmod_install_result::*_override` fields and the
/// CLI/log line), and none of the checks writes to the store: a rejected update
/// leaves the previous install untouched.
struct rfmod_install_options {
    /// Require a signature from one of `trusted_keys` (a signed package with
    /// an unknown key is rejected too).
    bool require_signature{false};
    /// Trust anchors used by the package verification step.
    std::vector<rfmod_trusted_key> trusted_keys{};
    /// Publish the per-mod install receipt (`<store>/mods/<id>/<version>.json`)
    /// after the batch commit and blob-table publish. `false` keeps the CAS
    /// writes but records nothing (tools/tests that only want the blobs) and
    /// also disables the update checks below: without a receipt there is no
    /// installed state to compare against.
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
    /// Signature continuity policy applied to updates.
    rfmod_signature_policy signature_policy{rfmod_signature_policy::strict};
    /// Explicit signer transitions allowed for a mod id (rotation allowlist).
    std::vector<rfmod_key_rotation> rotations{};
    /// Accept a version lower than the highest installed version.
    bool allow_downgrade{false};
    /// Accept the same `(id, version)` with a different archive digest.
    bool replace_same_version{false};
    /// Confirm a legacy (receipt v1 / unpinned) install so the next update may
    /// establish the signer pin. Without it, updating a store installed before
    /// signer pinning is rejected until an administrator confirms.
    bool accept_unpinned{false};
    /// Publish/refresh the signer pin (`<store>/mods/pins/<id>.json`).
    bool write_pin{true};
    /// Recorded in the receipt v2 (audit). Empty means "unknown" (a receipt
    /// migrated from v1 has no installer version either).
    std::string installer_version{};
    /// Recorded in the receipt v2 as an RFC 3339 UTC timestamp. Empty uses the
    /// current time; tests pass a fixed value so receipt bytes are stable.
    std::string installed_at{};
    /// Remove stale staging directories left by a crashed install before
    /// starting (a staging directory of a live process is never touched).
    bool prune_staging{true};
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
    /// Hex64 fingerprint of the verifying key, empty when unknown.
    std::string public_key_fingerprint{};
    /// Policy text recorded in the receipt: `strict`, `trusted-any` or
    /// `unsigned` (see `receipt_policy_text`).
    std::string receipt_policy{};
    mod_origin origin{mod_origin::user};
    /// `<store>/mods/<id>/<version>.json` when the receipt was written; empty
    /// when `write_receipt` is `false` (or on failure, which is an error).
    /// For an idempotent reinstall of an already installed archive this is the
    /// existing receipt and nothing was rewritten.
    std::string receipt_path{};
    /// True when the exact archive digest was already installed (no-op
    /// reinstall): no blob, table, receipt or pin write happened.
    bool already_installed{false};
    /// True when this install established/refreshed the signer pin.
    bool signer_pinned{false};
    /// True when the pinned signer changed through a rotation allowlist entry
    /// or the `trusted-any` policy (always audited).
    bool signer_rotated{false};
    /// `<store>/mods/pins/<id>.json` when a pin was written.
    std::string signer_pin_path{};
    /// True when `allow_downgrade` was required for this install.
    bool downgrade_override{false};
    /// True when `replace_same_version` was required for this install.
    bool same_version_override{false};
    /// True when `accept_unpinned` confirmed a legacy install.
    bool unpinned_confirmed{false};
    /// Stale staging directories removed before this install.
    std::size_t pruned_staging{0};
};

/// One installed member recorded in a `refrain.mod-receipt/1` or `/2` receipt.
struct receipt_file {
    std::string path{};
    std::uint64_t size{0};
    data::digest32 blake3{};
    std::uint32_t mode{0644};
};

/// Receipt schema versions understood by `read_receipt`.
inline constexpr std::string_view mod_receipt_schema_v1{
    "refrain.mod-receipt/1"};
inline constexpr std::string_view mod_receipt_schema_v2{
    "refrain.mod-receipt/2"};
/// Signer pin schema (`<store>/mods/pins/<id>.json`).
inline constexpr std::string_view mod_signer_pin_schema{
    "refrain.mod-signer-pin/1"};

/// Validated `<store>/mods/<id>/<version>.json` install receipt.
///
/// The receipt is the runtime's only external-mod load source and a content GC
/// root: `scan_installed()` parses it to build the runtime catalog and its
/// `files[].blake3` digests keep the installed blobs alive before any
/// resolution index exists, so `content gc` must keep those blobs/segments.
///
/// Schema v2 adds the signer identity (`public_key_fingerprint`), the policy
/// that accepted the install, and the audit fields `installed_at` /
/// `installer_version`. A v1 receipt parses into the same struct with
/// `schema_version == 1`, `policy == "legacy"` and an empty fingerprint; it
/// counts as unpinned until the signer is confirmed.
struct mod_receipt {
    std::uint32_t schema_version{2};
    std::string id{};
    std::string version{};
    std::uint32_t abi{0};
    data::digest32 archive_blake3{};
    data::digest32 manifest_blake3{};
    bool signed_package{false};
    bool trusted{false};
    std::string key_id{};
    /// Hex64 fingerprint of the signing key (v2, empty when unknown).
    std::string public_key_fingerprint{};
    /// `strict` | `trusted-any` | `unsigned` | `legacy` (v2; see
    /// `rfmod_signature_policy` and the receipt format comment).
    std::string policy{};
    /// RFC 3339 UTC install timestamp (v2; empty for a migrated v1 receipt).
    std::string installed_at{};
    /// Installer version string (v2; empty for a migrated v1 receipt).
    std::string installer_version{};
    /// Host-observed install tier (missing field in an older receipt = user).
    mod_origin origin{mod_origin::user};
    std::vector<receipt_file> files{};

    /// True when this receipt pins a signer: schema v2, signed package, a key
    /// id and a policy that confirmed the signer. A receipt migrated from v1
    /// keeps `policy == "legacy"` and is therefore *not* pinned: migrating the
    /// document does not confirm who signed the installed archive.
    [[nodiscard]] auto pinned() const noexcept -> bool {
        return schema_version >= 2U && signed_package && !key_id.empty() &&
               policy != "legacy";
    }
};

/// Parse and strictly validate one install receipt.
///
/// Checks the JSON object, a known schema (`refrain.mod-receipt/1` or `/2`),
/// every required top-level field/type, SemVer `version`, integer `abi`, 64-hex
/// `archive_blake3`/`manifest_blake3`, the v2 signer fields, and every
/// `files[]` entry's safe relative UTF-8 `path`, non-negative `size`, 64-hex
/// `blake3` and (when present) 0..0777 `mode`. A corrupt/foreign receipt is
/// `parse_error` (the message names `path`), never silently skipped.
[[nodiscard]] auto read_receipt(std::string_view path) -> result<mod_receipt>;

/// Sorted `<store>/mods/<id>/<version>.json` receipt paths.
///
/// A missing `<store>/mods` directory is an empty list; missing subdirectories
/// and non-`.json`/non-regular entries are skipped. The reserved
/// `<store>/mods/pins` directory and hidden (`.`-prefixed) directories such as
/// staging/trash are never treated as mod ids. Read each path with
/// `read_receipt` before using it as a GC root.
[[nodiscard]] auto list_receipt_paths(std::string_view store_root)
    -> result<std::vector<std::string>>;

/// `list_receipt_paths` restricted to one mod id, sorted by receipt path.
[[nodiscard]] auto list_receipt_paths_for(std::string_view store_root,
                                          std::string_view id)
    -> result<std::vector<std::string>>;

/// Canonical receipt path `<store>/mods/<id>/<version>.json`.
///
/// Both components are validated as safe single path segments
/// (`invalid_argument` otherwise), so the result can be used for reads and the
/// recursive uninstall. The path does not have to exist.
[[nodiscard]] auto receipt_path(std::string_view store_root,
                                std::string_view id, std::string_view version)
    -> result<std::string>;

/// Read the signer pin of one mod id, if a pin file exists.
///
/// A pin file is validated strictly (`parse_error` on damage); a missing
/// `<store>/mods/pins` directory or pin file is `nullopt`. The id is validated
/// as a safe path segment (`invalid_argument`).
[[nodiscard]] auto read_signer_pin(std::string_view store_root,
                                   std::string_view id)
    -> result<std::optional<mod_signer_pin>>;

/// Atomically write `<store>/mods/pins/<id>.json` (schema
/// `refrain.mod-signer-pin/1`). Used by install, uninstall (`keep_signer_pin`)
/// and the administrative `mods pin` command.
[[nodiscard]] auto write_signer_pin(std::string_view store_root,
                                    const mod_signer_pin &pin) -> status;

/// Remove the signer pin of `id`. A missing pin is a successful no-op.
[[nodiscard]] auto clear_signer_pin(std::string_view store_root,
                                    std::string_view id) -> status;

/// Fully verify the package (`verify_rfmod`), then install it into the content
/// store as one transaction: staging unpack, CAS commit, receipt, signer pin.
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
/// Transaction order (a crash at any point leaves the previous install usable):
///  1. prune stale `<store>/mods/.staging/*` directories (a crashed install's
///     debris; a staging directory owned by a live process is never touched);
///  2. verify the package in memory and check the update policy (signer
///     continuity, version monotonicity, same-version digest);
///  3. unpack the verified members into
///     `<store>/mods/.staging/<id>-<version>-<pid>-<n>/` and re-check every
///     digest;
///  4. ingest every member through one CAS batch and commit it;
///  5. publish the merged blob table and re-encoded resolution indexes;
///  6. publish the receipt v2 (the commit point: an install "exists" exactly
///     when its receipt exists);
///  7. publish the signer pin;
///  8. delete the staging directory.
///
/// Steps 1-2 write nothing, so a rejected package leaves no blob behind.
/// A failure between 4 and 6 leaves already-written blobs/segments behind
/// (append-only CAS semantics — harmless and collected by GC) and the staging
/// directory is removed; the next install prunes a crashed leftover.
///
/// Reinstalling the exact archive digest of an already installed `(id,
/// version)` is a no-op (`already_installed`), without rewriting the receipt
/// or the pin. `write_receipt = false` skips the receipt, pin and update
/// checks (a pure CAS ingestion used by tests/tools).
///
/// Receipt: once every member is in the CAS, its digest has been verified and
/// the batch committed, a deterministic JSON record is atomically written to
/// `<store>/mods/<id>/<version>.json`:
///
///   `schema` = "refrain.mod-receipt/2", `id`, `version`, `abi`,
///   `archive_blake3`, `manifest_blake3`, `signed`, `trusted`, `key_id`,
///   `public_key_fingerprint`, `policy` (`strict`, `trusted-any`, `unsigned`),
///   `installed_at`, `installer_version`, `origin` (`builtin`|`first_party`|
///   `user`), and `files` (one `path`/`size`/`blake3`/`mode` object per
///   installed member, sorted by path bytes).
///
/// `id`/`version` are re-checked as safe single path segments (non-empty, not
/// exactly "."/"..", no '/'/'\\'/control bytes; ordinary dots inside a name,
/// e.g. the valid mod id `a..b`, are allowed) *before the first CAS write*, so
/// an unsafe segment fails without leaving a blob behind. A table or receipt
/// write failure is `io_error`; blobs and sealed segments already written by
/// this call are kept (append-only CAS semantics — no rollback).
///
/// The receipt is the runtime's only external-mod load source
/// (`scan_installed`) and a content GC root: `content gc` treats every
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

/// Options for `uninstall_rfmod`.
struct rfmod_uninstall_options {
    /// Take `<store>/content/.lock` for the whole uninstall (default). A caller
    /// that already holds the store lock passes `false`.
    bool lock_store{true};
    /// Keep the signer pin when the last installed version of the id is
    /// removed. The default clears it, so a later reinstall starts with a
    /// fresh "first install" pin.
    bool keep_signer_pin{false};
};

/// Result of `uninstall_rfmod`.
struct rfmod_uninstall_result {
    std::string id{};
    /// Removed version texts, sorted ascending by SemVer then raw text.
    std::vector<std::string> versions{};
    /// True when the id has no installed version afterwards.
    bool last_version{false};
    /// True when the signer pin was deleted (default, last version only).
    bool signer_pin_cleared{false};
    /// True when the signer pin was written/kept instead of cleared.
    bool signer_pin_kept{false};
    std::string signer_pin_path{};
};

/// Remove installed receipts of one mod id, clearing the signer pin with the
/// last version.
///
/// `version` selects one installed version (exact receipt version text);
/// `nullopt` removes every installed version of the id. The receipts are the
/// only state the runtime and GC read, so removing them is the uninstall; the
/// CAS blobs stay in place and the next mark-sweep GC collects them once no
/// resolution index references them. A missing version/id is `not_found`.
/// A removed receipt is logged by the caller from the returned versions; the
/// library itself never writes an audit log.
[[nodiscard]] auto uninstall_rfmod(data::content_store &store,
                                   std::string_view id,
                                   std::optional<std::string_view> version = {},
                                   const rfmod_uninstall_options &options = {})
    -> result<rfmod_uninstall_result>;

/// One `migrate_receipts()` outcome.
struct receipt_migration_report {
    std::size_t scanned{0};
    std::size_t migrated{0};
    std::size_t current{0}; ///< already schema v2, untouched
    std::size_t failed{0};
};

/// Rewrite every receipt v1 under `<store>/mods` as schema v2.
///
/// A migrated receipt keeps every recorded value and gains
/// `policy = "legacy"`, an empty `public_key_fingerprint`, `installed_at` and
/// `installer_version`: the signer is *not* confirmed by a migration. The
/// runtime keeps accepting v1 receipts, so the migration is not required for a
/// working store; it makes the on-disk format uniform and marks the signer as
/// unconfirmed. The first update of an unpinned install is rejected unless the
/// administrator confirms the signer (`rfmod_install_options::accept_unpinned`
/// or `mods pin`). `apply == false` is a dry-run that only reports.
///
/// A corrupt receipt fails the report (`parse_error`) instead of being
/// rewritten; a failure while writing one receipt is reported per receipt in
/// `failed` and does not abort the pass.
[[nodiscard]] auto migrate_receipts(std::string_view store_root, bool apply)
    -> result<receipt_migration_report>;

} // namespace refrain::mod
