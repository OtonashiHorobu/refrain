// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/mod/rfmod.hpp>

#include <refrain/core/config.hpp>
#include <refrain/core/io.hpp>
#include <refrain/core/semver.hpp>
#include <refrain/core/version.hpp>
#include <refrain/data/json.hpp>
#include <refrain/data/path.hpp>
#include <refrain/data/signature.hpp>
#include <refrain/data/store_lock.hpp>
#include <refrain/mod/discovery.hpp>
#include <refrain/platform/process.hpp>

#include <archive.h>
#include <archive_entry.h>
#include <zstd.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <fmt/chrono.h>
#include <fmt/format.h>

namespace refrain::mod {
namespace {

namespace config = core::config;
namespace data = refrain::data;
namespace io = core::io;

// --- ustar layout constants (POSIX ustar, 512-byte header) ----------------
// The write side emits these fields; the read side only uses the magic/version
// span to reject non-tar streams before libarchive parses the members.
constexpr std::size_t tar_header_size{512};
constexpr std::size_t tar_name_offset{0};
constexpr std::size_t tar_name_size{100};
constexpr std::size_t tar_mode_offset{100};
constexpr std::size_t tar_mode_size{8};
constexpr std::size_t tar_uid_offset{108};
constexpr std::size_t tar_uid_size{8};
constexpr std::size_t tar_gid_offset{116};
constexpr std::size_t tar_gid_size{8};
constexpr std::size_t tar_size_offset{124};
constexpr std::size_t tar_size_size{12};
constexpr std::size_t tar_mtime_offset{136};
constexpr std::size_t tar_mtime_size{12};
constexpr std::size_t tar_checksum_offset{148};
constexpr std::size_t tar_checksum_size{8};
constexpr std::size_t tar_typeflag_offset{156};
constexpr std::size_t tar_magic_offset{257};
constexpr std::size_t tar_magic_size{6}; // "ustar\0"
constexpr std::size_t tar_version_offset{263};
constexpr std::size_t tar_version_size{2}; // "00"
constexpr std::size_t tar_prefix_offset{345};
constexpr std::size_t tar_prefix_size{155};

constexpr char ustar_magic[tar_magic_size] = {'u', 's', 't', 'a', 'r', '\0'};
constexpr char ustar_version[tar_version_size] = {'0', '0'};
constexpr char regular_typeflag = '0';
constexpr std::size_t tar_end_blocks{2};

// Magic+version accepted on the first header: POSIX ustar ("ustar\0" + "00")
// and GNU tar ("ustar  \0"). PAX archives use the POSIX spelling; GNU long
// name/link headers use the GNU spelling. This keeps the old strict rejection
// of non-tar/fake ustar headers while letting libarchive handle both families.
constexpr std::array<char, 8> posix_magic_version{'u', 's',  't', 'a',
                                                  'r', '\0', '0', '0'};
constexpr std::array<char, 8> gnu_magic_version{'u', 's', 't', 'a',
                                                'r', ' ', ' ', '\0'};

[[nodiscard]] auto annotate(error original, std::string_view context) -> error {
    if (original.message.find(context) != std::string::npos) {
        return original;
    }
    original.message = std::string{context} + ": " + original.message;
    return original;
}

/// True when every byte is zero (end-of-archive block).
[[nodiscard]] auto all_zero(std::span<const std::byte> data) noexcept -> bool {
    return std::all_of(data.begin(), data.end(),
                       [](std::byte value) { return value == std::byte{0}; });
}

/// Write a zero-padded octal value into `width` bytes: `width - 1` digits then
/// a NUL terminator (the convention used by our writer).
void write_octal(std::byte *field, std::size_t width, std::uint64_t value) {
    for (std::size_t index{0}; index < width; ++index) {
        field[index] = std::byte{'0'};
    }
    std::size_t position{width - 1};
    field[position] = std::byte{0};
    while (value > 0 && position > 0) {
        --position;
        field[position] =
            static_cast<std::byte>('0' + static_cast<char>(value & 0x7U));
        value >>= 3U;
    }
}

/// Header checksum: sum of every byte with the checksum field read as spaces.
[[nodiscard]] auto header_checksum(std::span<const std::byte> header) noexcept
    -> std::uint64_t {
    std::uint64_t sum{0};
    for (std::size_t index{0}; index < header.size(); ++index) {
        const bool in_checksum{index >= tar_checksum_offset &&
                               index < tar_checksum_offset + tar_checksum_size};
        sum += static_cast<std::uint64_t>(static_cast<unsigned char>(
            in_checksum ? std::byte{' '} : header[index]));
    }
    return sum;
}

/// Split a path into the ustar name/prefix pair. Returns nullopt when the path
/// cannot be represented without PAX/GNU extensions.
struct ustar_path {
    std::string_view prefix{};
    std::string_view name{};
};

[[nodiscard]] auto split_ustar_path(std::string_view path)
    -> std::optional<ustar_path> {
    if (path.empty()) {
        return std::nullopt;
    }
    if (path.size() <= tar_name_size) {
        return ustar_path{.prefix = {}, .name = path};
    }
    for (std::size_t index{1}; index < path.size(); ++index) {
        if (path[index] != '/') {
            continue;
        }
        const auto prefix{path.substr(0, index)};
        const auto name{path.substr(index + 1)};
        if (prefix.size() <= tar_prefix_size && !name.empty() &&
            name.size() <= tar_name_size) {
            return ustar_path{.prefix = prefix, .name = name};
        }
    }
    return std::nullopt;
}

/// Append one regular-file entry (header + zero-padded data).
auto append_entry(std::vector<std::byte> &tar, std::string_view path,
                  std::span<const std::byte> data) -> status {
    if (!data::is_safe_relative_path(path)) {
        return std::unexpected{make_error(error_code::invalid_argument,
                                          "rfmod: unsafe archive path '" +
                                              std::string{path} + "'")};
    }
    if (path.size() > 1024) {
        return std::unexpected{
            make_error(error_code::invalid_argument,
                       "rfmod: archive path is too long (max 1024 bytes)")};
    }
    const auto split{split_ustar_path(path)};
    if (!split.has_value()) {
        return std::unexpected{
            make_error(error_code::invalid_argument,
                       "rfmod: path '" + std::string{path} +
                           "' does not fit the ustar name/prefix fields")};
    }

    std::array<std::byte, tar_header_size> header{};
    std::memcpy(header.data() + tar_name_offset, split->name.data(),
                split->name.size());
    write_octal(header.data() + tar_mode_offset, tar_mode_size, 0644U);
    write_octal(header.data() + tar_uid_offset, tar_uid_size, 0U);
    write_octal(header.data() + tar_gid_offset, tar_gid_size, 0U);
    write_octal(header.data() + tar_size_offset, tar_size_size, data.size());
    write_octal(header.data() + tar_mtime_offset, tar_mtime_size, 0U);
    header[tar_typeflag_offset] = std::byte{regular_typeflag};
    std::memcpy(header.data() + tar_magic_offset, ustar_magic, tar_magic_size);
    std::memcpy(header.data() + tar_version_offset, ustar_version,
                tar_version_size);
    if (!split->prefix.empty()) {
        std::memcpy(header.data() + tar_prefix_offset, split->prefix.data(),
                    split->prefix.size());
    }
    const auto checksum{header_checksum(header)};
    // Checksum field: six octal digits, NUL, space.
    write_octal(header.data() + tar_checksum_offset, tar_checksum_size - 2U,
                checksum);
    header[tar_checksum_offset + tar_checksum_size - 2U] = std::byte{0};
    header[tar_checksum_offset + tar_checksum_size - 1U] = std::byte{' '};

    tar.insert(tar.end(), header.begin(), header.end());
    tar.insert(tar.end(), data.begin(), data.end());
    const auto padding{
        (rfmod_tar_block_size - (data.size() % rfmod_tar_block_size)) %
        rfmod_tar_block_size};
    tar.insert(tar.end(), padding, std::byte{0});
    return status_ok();
}

/// Deterministic zstd compression of one tar stream.
[[nodiscard]] auto compress_zstd(std::span<const std::byte> tar,
                                 std::int32_t level)
    -> result<std::vector<std::byte>> {
    const auto bound{ZSTD_compressBound(tar.size())};
    if (ZSTD_isError(bound) != 0) {
        return std::unexpected{make_error(
            error_code::unknown, "rfmod: zstd compression bound failed")};
    }
    std::vector<std::byte> compressed(bound);
    const auto maximum_level{ZSTD_maxCLevel()};
    const auto clamped{std::clamp(level, 1, maximum_level)};
    const auto written{ZSTD_compress(compressed.data(), compressed.size(),
                                     tar.data(), tar.size(), clamped)};
    if (ZSTD_isError(written) != 0) {
        return std::unexpected{
            make_error(error_code::unknown,
                       std::string{"rfmod: zstd compression failed: "} +
                           ZSTD_getErrorName(written))};
    }
    compressed.resize(written);
    return compressed;
}

/// Bounded zstd decompression (streaming, so a hostile frame without a content
/// size cannot force a large up-front allocation or bypass the byte limit).
[[nodiscard]] auto decompress_zstd(std::span<const std::byte> archive,
                                   std::size_t max_bytes)
    -> result<std::vector<std::byte>> {
    struct stream_guard {
        ZSTD_DStream *stream{nullptr};
        ~stream_guard() {
            if (stream != nullptr) {
                ZSTD_freeDStream(stream);
            }
        }
    };
    stream_guard guard{ZSTD_createDStream()};
    if (guard.stream == nullptr) {
        return std::unexpected{
            make_error(error_code::out_of_memory,
                       "rfmod: cannot allocate the zstd decompressor")};
    }
    const auto initialised{ZSTD_initDStream(guard.stream)};
    if (ZSTD_isError(initialised) != 0) {
        return std::unexpected{make_error(
            error_code::unknown, std::string{"rfmod: zstd init failed: "} +
                                     ZSTD_getErrorName(initialised))};
    }

    std::vector<std::byte> tar;
    tar.reserve(std::min<std::size_t>(max_bytes, 1024U * 1024U));
    std::array<std::byte, 64U * 1024U> chunk{};
    ZSTD_inBuffer input{archive.data(), archive.size(), 0};
    for (;;) {
        ZSTD_outBuffer output{chunk.data(), chunk.size(), 0};
        const auto before_input{input.pos};
        const auto remaining{
            ZSTD_decompressStream(guard.stream, &output, &input)};
        if (ZSTD_isError(remaining) != 0) {
            return std::unexpected{
                make_error(error_code::parse_error,
                           std::string{"rfmod: invalid zstd stream: "} +
                               ZSTD_getErrorName(remaining))};
        }
        if (output.pos > 0) {
            if (tar.size() + output.pos > max_bytes) {
                return std::unexpected{make_error(
                    error_code::parse_error, "rfmod: decompressed archive "
                                             "exceeds the configured limit")};
            }
            tar.insert(tar.end(), chunk.begin(),
                       chunk.begin() + static_cast<std::ptrdiff_t>(output.pos));
        }
        if (remaining == 0 && input.pos == input.size) {
            break; // frame complete and input consumed
        }
        if (input.pos == input.size && output.pos == 0) {
            return std::unexpected{make_error(error_code::parse_error,
                                              "rfmod: truncated zstd stream")};
        }
        if (input.pos == before_input && output.pos == 0) {
            return std::unexpected{
                make_error(error_code::parse_error,
                           "rfmod: malformed zstd stream (no progress)")};
        }
    }
    if (tar.size() > max_bytes) {
        return std::unexpected{make_error(
            error_code::parse_error,
            "rfmod: decompressed archive exceeds the configured limit")};
    }
    return tar;
}

// ---------------------------------------------------------------------------
// staging helpers (owning file set shared by build/pack)
// ---------------------------------------------------------------------------

struct owned_file {
    std::string path{};
    std::vector<std::byte> data{};
};

[[nodiscard]] auto to_inputs(const std::vector<owned_file> &files)
    -> std::vector<rfmod_input> {
    std::vector<rfmod_input> inputs;
    inputs.reserve(files.size());
    for (const auto &file : files) {
        inputs.push_back(rfmod_input{.path = file.path,
                                     .data = std::span<const std::byte>{
                                         file.data.data(), file.data.size()}});
    }
    return inputs;
}

// --- pack_mod_directory helpers -------------------------------------------

// Shipped top-level directories. `lib/` carries native entry libraries: an
// installed package is the only external load source, so the native loader
// materialises its entry from this packaged member.
constexpr std::array<std::string_view, 4> content_directories{"data", "assets",
                                                              "scripts", "lib"};
constexpr std::array<std::string_view, 2> root_metadata_files{"LICENSE",
                                                              "NOTICE"};

/// Read the shippable members of a mod directory through the single package
/// walker (`refrain::mod::collect_package_files`, shared with the builtin
/// image builder). The returned set is re-sorted before it is written, so the
/// archive stays deterministic.
[[nodiscard]] auto collect_mod_files(const std::string &root,
                                     const rfmod_write_options &options,
                                     std::vector<owned_file> &files) -> status {
    auto collected{refrain::mod::collect_package_files(
        root, refrain::mod::package_collect_options{
                  .error_prefix = "rfmod pack",
                  .roots = content_directories,
                  .extra_root_files = root_metadata_files,
                  .max_file_bytes = options.max_file_bytes,
                  .max_files = options.max_files,
                  // The package manifest is rebuilt below: it hashes every
                  // other member and cannot hash itself.
                  .include_manifest = false,
                  .skip_dotfiles = true,
                  // A symlink in a source tree is skipped, not followed: it
                  // must never leak a file from outside the mod directory.
                  .reject_symlinks = false,
                  .reject_special_files = false})};
    if (!collected) {
        return std::unexpected{std::move(collected.error())};
    }
    files.reserve(files.size() + collected->size());
    for (auto &file : *collected) {
        files.push_back(owned_file{.path = std::move(file.path),
                                   .data = std::move(file.data)});
    }
    return status_ok();
}

// ---------------------------------------------------------------------------
// package manifest: [files] / [packaging] / [signature]
// ---------------------------------------------------------------------------

// Packaging tool identity recorded in `[packaging]`. `tool_version` is the
// pure release SemVer, never `version::string()` (which appends the git
// branch/commit and would make the manifest non-reproducible).
constexpr std::string_view rfmod_tool_name{"refrain-tools"};
const std::string_view rfmod_tool_version{refrain::version::release_version};

constexpr std::array<std::string_view, 3> file_record_keys{"size", "blake3",
                                                           "mode"};
constexpr std::array<std::string_view, 5> packaging_keys{
    "tool", "tool_version", "zstd_level", "deterministic", "format_version"};
constexpr std::array<std::string_view, 3> signature_keys{"scheme", "key_id",
                                                         "sig"};
constexpr std::array<std::string_view, 3> package_table_names{
    "files", "packaging", "signature"};

/// Byte view of text (BLAKE3/Ed25519 take byte spans).
[[nodiscard]] auto as_bytes(std::string_view text) noexcept
    -> std::span<const std::byte> {
    return std::as_bytes(std::span{text});
}

/// True when `text` is exactly `length` lowercase hex characters.
[[nodiscard]] auto is_lower_hex(std::string_view text,
                                std::size_t length) noexcept -> bool {
    if (text.size() != length) {
        return false;
    }
    return std::all_of(text.begin(), text.end(), [](char character) {
        return (character >= '0' && character <= '9') ||
               (character >= 'a' && character <= 'f');
    });
}

/// A `[files]` path must satisfy the archive path rules, be well-formed UTF-8
/// (the package contract spells paths as UTF-8, and the reader rejects any
/// member name that is not) and additionally must not contain '"' (it is
/// emitted as a TOML basic-string key without escaping; every rejectable byte
/// here keeps packer and parser in exact agreement).
[[nodiscard]] auto is_package_path(std::string_view path) noexcept -> bool {
    return path.size() <= 1024 && data::is_safe_relative_path(path) &&
           data::is_valid_utf8(path) &&
           path.find('"') == std::string_view::npos;
}

/// A `key_id` is written into a TOML basic string verbatim: reject anything
/// that would need escaping (control bytes, '"' or '\\'). Key ids are
/// identifiers such as "refrain-community-2026".
[[nodiscard]] auto is_manifest_string_safe(std::string_view text) noexcept
    -> bool {
    return std::all_of(text.begin(), text.end(), [](char character) {
        const auto byte{static_cast<unsigned char>(character)};
        return byte >= 0x20U && byte != 0x7FU && character != '"' &&
               character != '\\';
    });
}

/// A table header discovered in the raw manifest bytes.
struct header_scan {
    /// Byte offset of the header line's first byte (the line start).
    std::size_t offset{0};
    /// Number of signature-table header lines found.
    std::size_t count{0};
};

/// True when the text between '[' and ']' names the `signature` table (bare,
/// quoted or whitespace-padded form).
[[nodiscard]] auto is_signature_header_key(std::string_view inner) noexcept
    -> bool {
    const auto trim = [](std::string_view text) {
        const auto first{text.find_first_not_of(" \t")};
        if (first == std::string_view::npos) {
            return std::string_view{};
        }
        const auto last{text.find_last_not_of(" \t")};
        return text.substr(first, last - first + 1);
    };
    auto key{trim(inner)};
    if (key.size() >= 2 && (key.front() == '"' || key.front() == '\'') &&
        key.back() == key.front()) {
        key = key.substr(1, key.size() - 2);
    }
    // `[signature]`, `[ signature ]` and `["signature"]` all name the
    // signature table; a dotted sub-table header (`[signature.foo]`) counts as
    // one too, so a forged/extra signature block cannot hide in the prefix.
    const auto dot{key.find('.')};
    if (dot != std::string_view::npos) {
        key = trim(key.substr(0, dot));
    }
    return key == "signature";
}

/// Scan the manifest for signature-table header lines.
///
/// This is a raw byte scan, deliberately independent of the TOML parser: the
/// signed prefix is defined by the *last* `[signature]` header line, so both
/// packer and verifier must locate that line the same way. (A line starting
/// with `[signature]` inside a multi-line string is counted as a header; mod
/// manifests have no reason to embed one, and rejecting is the safe reading of
/// "line-start `[signature]` is forgery".)
[[nodiscard]] auto scan_manifest_headers(std::string_view text) noexcept
    -> header_scan {
    header_scan scan;
    std::size_t offset{0};
    while (offset < text.size()) {
        const auto newline{text.find('\n', offset)};
        const auto end{newline == std::string_view::npos ? text.size()
                                                         : newline};
        auto line{text.substr(offset, end - offset)};
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) {
            line.remove_prefix(1);
        }
        if (!line.empty() && line.front() == '[') {
            const auto close{line.find(']')};
            if (close != std::string_view::npos &&
                is_signature_header_key(line.substr(1, close - 1))) {
                ++scan.count;
                scan.offset = offset;
            }
        }
        offset = newline == std::string_view::npos ? text.size() : newline + 1;
    }
    return scan;
}

/// True when a TOML table header line starts at/after `offset`.
[[nodiscard]] auto has_table_header_after(std::string_view text,
                                          std::size_t offset) noexcept -> bool {
    while (offset < text.size()) {
        const auto newline{text.find('\n', offset)};
        const auto end{newline == std::string_view::npos ? text.size()
                                                         : newline};
        auto line{text.substr(offset, end - offset)};
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) {
            line.remove_prefix(1);
        }
        if (!line.empty() && line.front() == '[') {
            return true;
        }
        offset = newline == std::string_view::npos ? text.size() : newline + 1;
    }
    return false;
}

// --- typed package-manifest getters (field-level diagnostics) --------------

[[nodiscard]] auto package_string(const config::document &doc,
                                  std::span<const std::string_view> path,
                                  std::string_view field)
    -> result<std::string> {
    auto value{doc.get_string_at(path)};
    if (!value) {
        if (value.error().code == error_code::not_found) {
            return std::unexpected{make_error(
                error_code::parse_error,
                fmt::format("mod package: missing required field '{}'",
                            field))};
        }
        return std::unexpected{make_error(
            error_code::parse_error,
            fmt::format("mod package: '{}' must be a string", field))};
    }
    return value;
}

[[nodiscard]] auto package_int64(const config::document &doc,
                                 std::span<const std::string_view> path,
                                 std::string_view field)
    -> result<std::int64_t> {
    auto value{doc.get_int64_at(path)};
    if (!value) {
        if (value.error().code == error_code::not_found) {
            return std::unexpected{make_error(
                error_code::parse_error,
                fmt::format("mod package: missing required field '{}'",
                            field))};
        }
        return std::unexpected{make_error(
            error_code::parse_error,
            fmt::format("mod package: '{}' must be an integer", field))};
    }
    return value;
}

[[nodiscard]] auto package_bool(const config::document &doc,
                                std::span<const std::string_view> path,
                                std::string_view field) -> result<bool> {
    auto value{doc.get_bool_at(path)};
    if (!value) {
        if (value.error().code == error_code::not_found) {
            return std::unexpected{make_error(
                error_code::parse_error,
                fmt::format("mod package: missing required field '{}'",
                            field))};
        }
        return std::unexpected{make_error(
            error_code::parse_error,
            fmt::format("mod package: '{}' must be a boolean", field))};
    }
    return value;
}

/// Reject keys outside `allowed` in the table at `path`.
[[nodiscard]] auto
reject_unknown_keys(const config::document &doc,
                    std::span<const std::string_view> path,
                    std::span<const std::string_view> allowed,
                    std::string_view table) -> status {
    auto keys{doc.keys_at(path)};
    if (!keys) {
        return std::unexpected{keys.error()};
    }
    for (const auto &key : *keys) {
        const auto known{std::any_of(
            allowed.begin(), allowed.end(),
            [&key](std::string_view candidate) { return candidate == key; })};
        if (!known) {
            return std::unexpected{make_error(
                error_code::parse_error,
                fmt::format("mod package: unknown {} key '{}'", table, key))};
        }
    }
    return status_ok();
}

/// Parse + strictly validate `[files]` (sorted by TOML map order, which is the
/// byte order the packer writes).
[[nodiscard]] auto read_files_table(const config::document &doc,
                                    std::vector<rfmod_file_record> &out)
    -> status {
    constexpr std::array<std::string_view, 1> files_path{"files"};
    const auto kind{doc.kind_at(files_path)};
    if (kind == config::value_kind::missing) {
        return std::unexpected{make_error(
            error_code::parse_error,
            "mod package: manifest has no [files] table (a bare container from "
            "build_rfmod is not an installable package)")};
    }
    if (kind != config::value_kind::table) {
        return std::unexpected{make_error(
            error_code::parse_error, "mod package: [files] must be a table")};
    }
    auto paths{doc.keys_at(files_path)};
    if (!paths) {
        return std::unexpected{paths.error()};
    }
    out.reserve(paths->size());
    for (const auto &path : *paths) {
        if (!is_package_path(path)) {
            return std::unexpected{make_error(
                error_code::parse_error,
                fmt::format("mod package: unsafe [files] path '{}'", path))};
        }
        if (path == rfmod_manifest_path) {
            return std::unexpected{make_error(
                error_code::parse_error,
                "mod package: [files] must not list manifest.toml (it cannot "
                "contain its own hash; the signature covers it)")};
        }
        const std::array<std::string_view, 2> record_path{"files", path};
        if (doc.kind_at(record_path) != config::value_kind::table) {
            return std::unexpected{make_error(
                error_code::parse_error,
                fmt::format("mod package: [files] entry '{}' must be a table",
                            path))};
        }
        if (auto known{reject_unknown_keys(doc, record_path, file_record_keys,
                                           "[files] entry")};
            !known) {
            return known;
        }

        const std::array<std::string_view, 3> size_path{"files", path, "size"};
        auto size{package_int64(doc, size_path,
                                fmt::format("[files].{}.size", path))};
        if (!size) {
            return std::unexpected{size.error()};
        }
        if (*size < 0) {
            return std::unexpected{make_error(
                error_code::parse_error,
                fmt::format("mod package: [files] entry '{}' has a negative "
                            "size",
                            path))};
        }

        const std::array<std::string_view, 3> digest_path{"files", path,
                                                          "blake3"};
        auto hex{package_string(doc, digest_path,
                                fmt::format("[files].{}.blake3", path))};
        if (!hex) {
            return std::unexpected{hex.error()};
        }
        if (!is_lower_hex(*hex, 64)) {
            return std::unexpected{make_error(
                error_code::parse_error,
                fmt::format(
                    "mod package: [files] entry '{}' has a non-lowercase "
                    "or malformed blake3 (64 lowercase hex expected)",
                    path))};
        }
        auto digest{data::parse_digest(*hex)};
        if (!digest) {
            return std::unexpected{
                make_error(error_code::parse_error,
                           fmt::format("mod package: [files] entry '{}': {}",
                                       path, digest.error().message))};
        }

        std::uint32_t mode{0644};
        const std::array<std::string_view, 3> mode_path{"files", path, "mode"};
        if (doc.contains_at(mode_path)) {
            auto value{package_int64(doc, mode_path,
                                     fmt::format("[files].{}.mode", path))};
            if (!value) {
                return std::unexpected{value.error()};
            }
            if (*value < 0 || *value > 0777) {
                return std::unexpected{make_error(
                    error_code::parse_error,
                    fmt::format("mod package: [files] entry '{}' has an "
                                "invalid mode (0..0777)",
                                path))};
            }
            mode = static_cast<std::uint32_t>(*value);
        }

        out.push_back(
            rfmod_file_record{.path = path,
                              .size = static_cast<std::uint64_t>(*size),
                              .blake3 = *digest,
                              .mode = mode});
    }
    return status_ok();
}

/// Validate `[packaging]`: all keys known and present, deterministic build, and
/// the container version this reader understands.
[[nodiscard]] auto read_packaging(const config::document &doc) -> status {
    constexpr std::array<std::string_view, 1> packaging_path{"packaging"};
    const auto kind{doc.kind_at(packaging_path)};
    if (kind == config::value_kind::missing) {
        return std::unexpected{
            make_error(error_code::parse_error,
                       "mod package: manifest has no [packaging] table")};
    }
    if (kind != config::value_kind::table) {
        return std::unexpected{
            make_error(error_code::parse_error,
                       "mod package: [packaging] must be a table")};
    }
    if (auto known{reject_unknown_keys(doc, packaging_path, packaging_keys,
                                       "[packaging]")};
        !known) {
        return known;
    }

    auto tool{package_string(
        doc, std::array<std::string_view, 2>{"packaging", "tool"},
        "[packaging].tool")};
    if (!tool) {
        return std::unexpected{tool.error()};
    }
    if (tool->empty()) {
        return std::unexpected{
            make_error(error_code::parse_error,
                       "mod package: [packaging].tool must not be empty")};
    }
    auto tool_version{package_string(
        doc, std::array<std::string_view, 2>{"packaging", "tool_version"},
        "[packaging].tool_version")};
    if (!tool_version) {
        return std::unexpected{tool_version.error()};
    }
    if (tool_version->empty()) {
        return std::unexpected{make_error(
            error_code::parse_error,
            "mod package: [packaging].tool_version must not be empty")};
    }
    auto level{package_int64(
        doc, std::array<std::string_view, 2>{"packaging", "zstd_level"},
        "[packaging].zstd_level")};
    if (!level) {
        return std::unexpected{level.error()};
    }
    if (*level < 1) {
        return std::unexpected{make_error(
            error_code::parse_error,
            "mod package: [packaging].zstd_level must be a positive integer")};
    }
    auto deterministic{package_bool(
        doc, std::array<std::string_view, 2>{"packaging", "deterministic"},
        "[packaging].deterministic")};
    if (!deterministic) {
        return std::unexpected{deterministic.error()};
    }
    if (!*deterministic) {
        return std::unexpected{
            make_error(error_code::parse_error,
                       "mod package: [packaging].deterministic must be true")};
    }
    auto version{package_int64(
        doc, std::array<std::string_view, 2>{"packaging", "format_version"},
        "[packaging].format_version")};
    if (!version) {
        return std::unexpected{version.error()};
    }
    if (*version != static_cast<std::int64_t>(rfmod_format_version)) {
        return std::unexpected{make_error(
            error_code::version_mismatch,
            fmt::format("mod package: container format_version {} is not "
                        "supported (this build reads version {})",
                        *version, rfmod_format_version))};
    }
    return status_ok();
}

/// Validate the optional `[mod].format_version` (same container version).
[[nodiscard]] auto read_mod_format_version(const config::document &doc)
    -> status {
    const std::array<std::string_view, 2> path{"mod", "format_version"};
    if (!doc.contains_at(path)) {
        return status_ok();
    }
    auto version{package_int64(doc, path, "[mod].format_version")};
    if (!version) {
        return std::unexpected{version.error()};
    }
    if (*version != static_cast<std::int64_t>(rfmod_format_version)) {
        return std::unexpected{make_error(
            error_code::version_mismatch,
            fmt::format("mod package: [mod].format_version {} is not supported "
                        "(this build reads version {})",
                        *version, rfmod_format_version))};
    }
    return status_ok();
}

/// Validate `[signature]` when the raw text has a signature header line.
[[nodiscard]] auto read_signature(const config::document &doc,
                                  std::string_view text,
                                  const header_scan &scan)
    -> result<std::optional<rfmod_signature_info>> {
    constexpr std::array<std::string_view, 1> signature_path{"signature"};
    if (scan.count == 0) {
        if (doc.contains_at(signature_path)) {
            return std::unexpected{make_error(
                error_code::parse_error,
                "mod package: [signature] must be a trailing [signature] "
                "table, not an inline/dotted key")};
        }
        return std::optional<rfmod_signature_info>{};
    }
    if (scan.count > 1) {
        return std::unexpected{make_error(
            error_code::parse_error,
            "mod package: manifest declares more than one [signature] table")};
    }
    if (doc.kind_at(signature_path) != config::value_kind::table) {
        return std::unexpected{
            make_error(error_code::parse_error,
                       "mod package: [signature] must be a table")};
    }
    if (auto known{reject_unknown_keys(doc, signature_path, signature_keys,
                                       "[signature]")};
        !known) {
        return std::unexpected{known.error()};
    }
    auto scheme{package_string(
        doc, std::array<std::string_view, 2>{"signature", "scheme"},
        "[signature].scheme")};
    if (!scheme) {
        return std::unexpected{scheme.error()};
    }
    auto key_id{package_string(
        doc, std::array<std::string_view, 2>{"signature", "key_id"},
        "[signature].key_id")};
    if (!key_id) {
        return std::unexpected{key_id.error()};
    }
    if (key_id->empty()) {
        return std::unexpected{
            make_error(error_code::parse_error,
                       "mod package: [signature].key_id must not be empty")};
    }
    auto sig_hex{
        package_string(doc, std::array<std::string_view, 2>{"signature", "sig"},
                       "[signature].sig")};
    if (!sig_hex) {
        return std::unexpected{sig_hex.error()};
    }
    if (!is_lower_hex(*sig_hex, 128)) {
        return std::unexpected{
            make_error(error_code::invalid_argument,
                       "mod package: [signature].sig must be 128 lowercase hex "
                       "characters")};
    }
    // The signature block must be the last table of the file: any other table
    // after its header line would contradict "one signature over one
    // manifest".
    const auto newline{text.find('\n', scan.offset)};
    if (has_table_header_after(text, newline == std::string_view::npos
                                         ? text.size()
                                         : newline + 1U)) {
        return std::unexpected{make_error(error_code::parse_error,
                                          "mod package: [signature] must be "
                                          "the last table in manifest.toml")};
    }
    return std::optional<rfmod_signature_info>{
        rfmod_signature_info{.scheme = std::move(*scheme),
                             .key_id = std::move(*key_id),
                             .sig_hex = std::move(*sig_hex)}};
}

/// A fully validated package manifest (raw text + parsed tables).
struct package_manifest {
    std::string text{};
    std::size_t signature_header_offset{0};
    bool has_signature{false};
    mod::mod_manifest manifest{};
    std::vector<rfmod_file_record> files{};
    std::optional<rfmod_signature_info> signature{};

    /// Canonical signed bytes: everything before the `[signature]` header line
    /// (empty for unsigned packages, where nothing is signed).
    [[nodiscard]] auto signed_prefix() const noexcept -> std::string_view {
        return std::string_view{text}.substr(0, signature_header_offset);
    }
};

/// Parse and strictly validate a package `manifest.toml` text.
[[nodiscard]] auto parse_package_manifest(std::string text,
                                          std::string_view origin)
    -> result<package_manifest> {
    package_manifest package;
    const auto scan{scan_manifest_headers(text)};
    package.has_signature = scan.count > 0;
    package.signature_header_offset = scan.offset;

    auto doc{config::document::parse_text(text, origin)};
    if (!doc) {
        return std::unexpected{std::move(doc.error())};
    }
    auto manifest{mod::parse_mod_manifest(
        *doc, mod::manifest_parse_options{.allow_package_tables = true})};
    if (!manifest) {
        return std::unexpected{std::move(manifest.error())};
    }
    package.manifest = std::move(*manifest);
    if (auto version{read_mod_format_version(*doc)}; !version) {
        return std::unexpected{std::move(version.error())};
    }
    // [files] first: it is the table a bare container lacks, so the diagnostic
    // names the missing package structure rather than a secondary symptom.
    if (auto files{read_files_table(*doc, package.files)}; !files) {
        return std::unexpected{std::move(files.error())};
    }
    if (auto packaging{read_packaging(*doc)}; !packaging) {
        return std::unexpected{std::move(packaging.error())};
    }
    auto signature{read_signature(*doc, text, scan)};
    if (!signature) {
        return std::unexpected{std::move(signature.error())};
    }
    package.signature = std::move(*signature);
    package.text = std::move(text);
    return package;
}

/// Coverage both ways between the archive members and `[files]`: every member
/// (except manifest.toml) matches its record's size and BLAKE3, and every
/// record has a member. This is the core package-integrity check.
[[nodiscard]] auto check_coverage(const rfmod_reader &reader,
                                  const std::vector<rfmod_file_record> &files)
    -> status {
    std::map<std::string, const rfmod_file_record *, std::less<>> by_path;
    for (const auto &record : files) {
        by_path.emplace(record.path, &record);
    }
    for (const auto &entry : reader.entries()) {
        if (entry.path == rfmod_manifest_path) {
            continue;
        }
        const auto found{by_path.find(entry.path)};
        if (found == by_path.end()) {
            return std::unexpected{make_error(
                error_code::parse_error,
                fmt::format("mod package: archive member '{}' is missing from "
                            "[files]",
                            entry.path))};
        }
        const auto &record{*found->second};
        if (record.size != entry.size) {
            return std::unexpected{make_error(
                error_code::parse_error,
                fmt::format("mod package: [files] records size {} for '{}' but "
                            "the archive member is {} bytes",
                            record.size, entry.path, entry.size))};
        }
        auto bytes{reader.read(entry.path)};
        if (!bytes) {
            return std::unexpected{std::move(bytes.error())};
        }
        if (data::blake3(*bytes) != record.blake3) {
            return std::unexpected{make_error(
                error_code::parse_error,
                fmt::format("mod package: BLAKE3 of '{}' does not match its "
                            "[files] record",
                            entry.path))};
        }
    }
    for (const auto &record : files) {
        if (reader.find(record.path) == nullptr) {
            return std::unexpected{make_error(
                error_code::parse_error,
                fmt::format("mod package: [files] lists '{}' but the archive "
                            "has no such member",
                            record.path))};
        }
    }
    return status_ok();
}

/// Apply the trust policy and fill the signature-related result
/// fields.
[[nodiscard]] auto apply_signature_policy(const package_manifest &package,
                                          const rfmod_verify_options &options,
                                          rfmod_verification &out) -> status {
    out.signed_package = package.signature.has_value();
    if (!package.signature) {
        if (options.require_signature) {
            return std::unexpected{make_error(
                error_code::not_found,
                "mod package: manifest is unsigned but require_signature is "
                "set")};
        }
        return status_ok();
    }

    const auto &info{*package.signature};
    out.signature = info;
    out.key_id = info.key_id;
    if (info.scheme != "ed25519") {
        return std::unexpected{make_error(
            error_code::invalid_argument,
            fmt::format("mod package: unsupported signature scheme '{}' "
                        "(expected ed25519)",
                        info.scheme))};
    }
    auto signature{data::parse_signature_hex(info.sig_hex)};
    if (!signature) {
        return std::unexpected{signature.error()};
    }

    const auto trusted{
        std::find_if(options.trusted_keys.begin(), options.trusted_keys.end(),
                     [&info](const rfmod_trusted_key &candidate) {
                         return candidate.key_id == info.key_id;
                     })};
    if (trusted != options.trusted_keys.end()) {
        auto verified{data::verify(as_bytes(package.signed_prefix()),
                                   *signature, trusted->public_key)};
        if (!verified) {
            return std::unexpected{make_error(
                error_code::parse_error,
                fmt::format("mod package: Ed25519 verification failed for key "
                            "'{}': {}",
                            info.key_id, verified.error().message))};
        }
        out.trusted = true;
        out.public_key_fingerprint =
            public_key_fingerprint(trusted->public_key);
        return status_ok();
    }

    if (!options.trusted_keys.empty()) {
        return std::unexpected{make_error(
            error_code::not_found,
            fmt::format("mod package: untrusted key_id '{}' (no matching "
                        "trusted key configured)",
                        info.key_id))};
    }
    if (options.require_signature) {
        return std::unexpected{make_error(
            error_code::invalid_argument,
            "mod package: require_signature is set but no trusted keys are "
            "configured")};
    }
    // Signed by an unknown key with no trust anchors configured: accept the
    // package (third-party distribution), but never mark it trusted.
    return status_ok();
}

} // namespace

// ---------------------------------------------------------------------------
// build / write
// ---------------------------------------------------------------------------

auto build_rfmod(std::span<const rfmod_input> inputs,
                 const rfmod_write_options &options)
    -> result<std::vector<std::byte>> {
    if (inputs.size() > options.max_files) {
        return std::unexpected{make_error(
            error_code::invalid_argument,
            "rfmod: too many files (" + std::to_string(inputs.size()) + " > " +
                std::to_string(options.max_files) + ")")};
    }
    std::vector<const rfmod_input *> ordered;
    ordered.reserve(inputs.size());
    for (const auto &input : inputs) {
        ordered.push_back(&input);
    }
    std::sort(ordered.begin(), ordered.end(),
              [](const rfmod_input *lhs, const rfmod_input *rhs) {
                  return lhs->path < rhs->path;
              });
    for (std::size_t index{1}; index < ordered.size(); ++index) {
        if (ordered[index - 1]->path == ordered[index]->path) {
            return std::unexpected{
                make_error(error_code::invalid_argument,
                           "rfmod: duplicate archive path '" +
                               ordered[index]->path + "'")};
        }
    }

    std::vector<std::byte> tar;
    tar.reserve(1024U + tar_end_blocks * tar_header_size);
    for (const auto *input : ordered) {
        if (input->data.size() > options.max_file_bytes) {
            return std::unexpected{
                make_error(error_code::invalid_argument,
                           "rfmod: file '" + input->path +
                               "' exceeds the configured size limit")};
        }
        if (auto appended = append_entry(tar, input->path, input->data);
            !appended) {
            return std::unexpected{std::move(appended.error())};
        }
    }
    // Two zero blocks terminate the archive.
    tar.insert(tar.end(), tar_end_blocks * tar_header_size, std::byte{0});

    return compress_zstd(tar, options.zstd_level);
}

auto write_rfmod(std::string_view path, std::span<const rfmod_input> inputs,
                 const rfmod_write_options &options) -> result<data::digest32> {
    if (path.empty()) {
        return std::unexpected{make_error(error_code::invalid_argument,
                                          "rfmod: empty output path")};
    }
    auto archive{build_rfmod(inputs, options)};
    if (!archive) {
        return std::unexpected{std::move(archive.error())};
    }
    if (auto written =
            io::atomic_write_file(path, std::span<const std::byte>{*archive});
        !written) {
        return std::unexpected{std::move(written.error())};
    }
    return data::blake3(*archive);
}

namespace {

/// Build the shipped manifest: source text + `[files]` + `[packaging]`
/// (+ `[signature]` when signing). The returned text is final (it is hashed
/// and signed as-is, never re-serialized).
[[nodiscard]] auto format_package_manifest(
    std::string_view source, std::span<const rfmod_file_record> records,
    const rfmod_write_options &options) -> result<std::string> {
    // Trailing whitespace is not part of the canonical prefix: the packer
    // appends its own deterministic block, so a file that merely differs in
    // trailing newlines still produces the same package.
    auto trimmed{source};
    while (!trimmed.empty()) {
        const auto character{trimmed.back()};
        if (character != '\n' && character != '\r' && character != ' ' &&
            character != '\t') {
            break;
        }
        trimmed.remove_suffix(1);
    }
    if (trimmed.empty()) {
        return std::unexpected{make_error(
            error_code::parse_error,
            "rfmod pack: manifest.toml is empty after trimming trailing "
            "whitespace")};
    }

    std::string out{trimmed};
    out += "\n\n[files]\n";
    for (const auto &record : records) {
        fmt::format_to(
            std::back_inserter(out),
            "\"{}\" = {{ size = {}, blake3 = \"{}\", mode = 0o{:o} }}\n",
            record.path, record.size, data::to_hex(record.blake3), record.mode);
    }
    out += "[packaging]\n";
    fmt::format_to(std::back_inserter(out),
                   "tool = \"{}\"\ntool_version = \"{}\"\nzstd_level = {}\n"
                   "deterministic = true\nformat_version = {}\n",
                   rfmod_tool_name, rfmod_tool_version, options.zstd_level,
                   rfmod_format_version);
    if (!options.signing) {
        return out; // unsigned package: [files] + [packaging] only
    }

    const auto &signing{*options.signing};
    if (signing.key_id.empty()) {
        return std::unexpected{
            make_error(error_code::invalid_argument,
                       "rfmod pack: signing key_id must not be empty")};
    }
    if (!is_manifest_string_safe(signing.key_id)) {
        return std::unexpected{make_error(
            error_code::invalid_argument,
            "rfmod pack: signing key_id must not contain double quotes, "
            "backslashes or control characters")};
    }
    // The separator blank line precedes the `[signature]` header line, so it
    // belongs to the canonical signed prefix (rfmod.hpp). Signing exactly
    // these bytes keeps packer and verifier on the same definition.
    out += "\n";
    const auto signature{data::sign(as_bytes(out), signing.secret_key)};
    fmt::format_to(std::back_inserter(out),
                   "[signature]\nscheme = \"ed25519\"\nkey_id = \"{}\"\n"
                   "sig = \"{}\"\n",
                   signing.key_id, data::to_hex(signature));
    return out;
}

} // namespace

auto pack_mod_directory(std::string_view mod_directory,
                        std::string_view output_path,
                        const rfmod_write_options &options)
    -> result<packed_rfmod> {
    if (mod_directory.empty()) {
        return std::unexpected{make_error(error_code::invalid_argument,
                                          "rfmod pack: empty mod directory")};
    }
    if (output_path.empty()) {
        return std::unexpected{make_error(error_code::invalid_argument,
                                          "rfmod pack: empty output path")};
    }
    const std::string root{mod_directory};
    if (!io::is_directory(root)) {
        return std::unexpected{make_error(
            error_code::not_found,
            fmt::format("rfmod pack: mod directory not found: {}", root))};
    }
    const auto manifest_path{data::join_path(root, rfmod_manifest_path)};
    if (!io::is_regular_file(manifest_path)) {
        return std::unexpected{make_error(
            error_code::not_found,
            fmt::format("rfmod pack: missing manifest.toml in '{}'", root))};
    }

    auto manifest_text{io::read_text(manifest_path)};
    if (!manifest_text) {
        return std::unexpected{annotate(std::move(manifest_text.error()),
                                        "rfmod pack: manifest.toml")};
    }
    auto manifest_doc{
        config::document::parse_text(*manifest_text, manifest_path)};
    if (!manifest_doc) {
        return std::unexpected{annotate(std::move(manifest_doc.error()),
                                        "rfmod pack: manifest.toml")};
    }
    // Packaging a package (or a signed package) again would nest/duplicate the
    // package tables: refuse with a directive message instead of letting the
    // kernel reject an "unknown top-level table".
    for (const auto &key : manifest_doc->keys("")) {
        const auto package_table{std::any_of(
            package_table_names.begin(), package_table_names.end(),
            [&key](std::string_view candidate) { return candidate == key; })};
        if (package_table) {
            return std::unexpected{make_error(
                error_code::invalid_argument,
                fmt::format("rfmod pack: '{}' already contains the package "
                            "table [{}]; pack a source mod directory, "
                            "not an already packed/signed package",
                            manifest_path, key))};
        }
    }
    // A line-start `[signature]` in the source bytes would be signed into the
    // prefix and then counted as a second signature block by verification;
    // reject it up front (forged/duplicate signature blocks).
    if (scan_manifest_headers(*manifest_text).count > 0) {
        return std::unexpected{make_error(
            error_code::invalid_argument,
            "rfmod pack: source manifest.toml contains a [signature] table "
            "(refusing to pack an already signed manifest)")};
    }
    // Validate the source manifest with the kernel parser before packing.
    auto manifest{mod::parse_mod_manifest(*manifest_doc)};
    if (!manifest) {
        return std::unexpected{
            annotate(std::move(manifest.error()), "rfmod pack: manifest.toml")};
    }

    // Content files only: manifest.toml is rebuilt below (it hashes every
    // other member, and cannot hash itself).
    std::vector<owned_file> files;
    if (auto collected = collect_mod_files(root, options, files); !collected) {
        return std::unexpected{std::move(collected.error())};
    }
    if (files.size() + 1 > options.max_files) {
        return std::unexpected{
            make_error(error_code::invalid_argument,
                       fmt::format("rfmod pack: too many files ({} > {})",
                                   files.size() + 1, options.max_files))};
    }
    // Every path below becomes a TOML basic-string key in [files], written
    // without escaping, so a name the verifier cannot read back (e.g. one
    // containing '"') would yield an archive that packs fine but can never be
    // installed. Reject those paths now with the same predicate the verifier
    // uses for [files] keys.
    for (const auto &file : files) {
        if (!is_package_path(file.path)) {
            return std::unexpected{make_error(
                error_code::invalid_argument,
                fmt::format("rfmod pack: path '{}' cannot be recorded as a "
                            "[files] key: paths must be safe archive paths of "
                            "at most 1024 bytes, valid UTF-8 and without '\"' "
                            "(TOML basic-string keys are written unescaped "
                            "until core::config::table_builder adds "
                            "escaping)",
                            file.path))};
        }
    }

    std::sort(files.begin(), files.end(),
              [](const owned_file &lhs, const owned_file &rhs) {
                  return lhs.path < rhs.path;
              });
    std::vector<rfmod_file_record> records;
    records.reserve(files.size());
    for (const auto &file : files) {
        records.push_back(rfmod_file_record{
            .path = file.path,
            .size = file.data.size(),
            .blake3 = data::blake3(
                std::span<const std::byte>{file.data.data(), file.data.size()}),
            // The deterministic tar writer emits 0644 for every member; the
            // record must describe what is actually inside the archive.
            .mode = 0644});
    }

    auto package_text{
        format_package_manifest(*manifest_text, records, options)};
    if (!package_text) {
        return std::unexpected{annotate(std::move(package_text.error()),
                                        "rfmod pack: manifest.toml")};
    }

    std::vector<owned_file> archive_files;
    archive_files.reserve(files.size() + 1);
    const auto manifest_payload{as_bytes(*package_text)};
    archive_files.push_back(
        owned_file{.path = std::string{rfmod_manifest_path},
                   .data = std::vector<std::byte>{manifest_payload.begin(),
                                                  manifest_payload.end()}});
    for (auto &file : files) {
        archive_files.push_back(std::move(file));
    }

    const auto inputs{to_inputs(archive_files)};
    auto archive{build_rfmod(inputs, options)};
    if (!archive) {
        return std::unexpected{std::move(archive.error())};
    }
    if (auto written = io::atomic_write_file(
            output_path, std::span<const std::byte>{*archive});
        !written) {
        return std::unexpected{std::move(written.error())};
    }

    packed_rfmod packed;
    packed.archive_path = std::string{output_path};
    packed.archive_digest = data::blake3(*archive);
    packed.file_count = archive_files.size();
    packed.total_bytes = 0;
    for (const auto &file : archive_files) {
        packed.total_bytes += file.data.size();
    }
    return packed;
}

// ---------------------------------------------------------------------------
// reader
// ---------------------------------------------------------------------------

namespace {

/// Owns one libarchive read handle. The project builds without exceptions, so
/// cleanup is a scope guard rather than a smart pointer with a custom deleter:
/// every error path below returns through this destructor.
class archive_reader {
  public:
    archive_reader() : handle_{archive_read_new()} {}
    ~archive_reader() {
        if (handle_ != nullptr) {
            (void)archive_read_free(handle_);
        }
    }

    archive_reader(const archive_reader &) = delete;
    auto operator=(const archive_reader &) -> archive_reader & = delete;
    archive_reader(archive_reader &&) = delete;
    auto operator=(archive_reader &&) -> archive_reader & = delete;

    [[nodiscard]] auto get() const noexcept -> archive * { return handle_; }

  private:
    archive *handle_{nullptr};
};

/// Build a `parse_error` from libarchive's current diagnostic.
[[nodiscard]] auto archive_failure(archive *handle, std::string_view context)
    -> error {
    const char *detail{archive_error_string(handle)};
    std::string message{"rfmod: "};
    message.append(context);
    if (detail != nullptr && detail[0] != '\0') {
        message.append(": ");
        message.append(detail);
    }
    return make_error(error_code::parse_error, message);
}

/// Human-readable name for a rejected archive entry type (directories are
/// skipped before this is reached, so they have no arm here).
[[nodiscard]] auto entry_type_name(std::uint32_t type) noexcept
    -> std::string_view {
    switch (type) {
    case AE_IFLNK:
        return "symbolic link";
    case AE_IFCHR:
        return "character device";
    case AE_IFBLK:
        return "block device";
    case AE_IFIFO:
        return "FIFO";
    case AE_IFSOCK:
        return "socket";
    default:
        return "non-regular file";
    }
}

} // namespace

struct rfmod_reader::impl {
    /// Member payloads in archive order (aligned with `entries`).
    std::vector<std::vector<std::byte>> payloads{};
    std::vector<rfmod_entry> entries{};
    std::map<std::string, std::size_t, std::less<>> index{};
    data::digest32 archive_digest{};
    std::size_t decompressed_size{0};

    /// Parse the decompressed tar with libarchive (in-memory, tar format only,
    /// no filters: zstd stays in `decompress_zstd`). Member so the private impl
    /// type stays encapsulated; the libarchive C API is confined here.
    [[nodiscard]] auto parse(std::span<const std::byte> tar,
                             const rfmod_read_options &options) -> status;
};

auto rfmod_reader::impl::parse(std::span<const std::byte> tar,
                               const rfmod_read_options &options) -> status {
    // Container framing: an archive is at least the two end-of-archive blocks,
    // ends with them (extra zero padding allowed), and starts with a POSIX
    // ustar or GNU tar magic/version. libarchive itself is lenient about a
    // missing end marker and trailing garbage; accepting those would silently
    // weaken the reader, so they stay rejected here.
    if (tar.size() < tar_end_blocks * tar_header_size) {
        return std::unexpected{
            make_error(error_code::parse_error,
                       "rfmod: truncated tar archive (missing end-of-archive "
                       "blocks)")};
    }
    if (all_zero(tar.first(tar_header_size))) {
        // The canonical empty archive is nothing but zero blocks.
        if (!all_zero(tar)) {
            return std::unexpected{
                make_error(error_code::parse_error,
                           "rfmod: tar data follows an end-of-archive marker")};
        }
        return status_ok();
    }
    if (!all_zero(tar.last(tar_end_blocks * tar_header_size))) {
        return std::unexpected{
            make_error(error_code::parse_error,
                       "rfmod: missing end-of-archive blocks or trailing data "
                       "(truncated tar)")};
    }
    const auto magic{tar.subspan(tar_magic_offset, posix_magic_version.size())};
    const bool posix{std::memcmp(magic.data(), posix_magic_version.data(),
                                 posix_magic_version.size()) == 0};
    const bool gnu{std::memcmp(magic.data(), gnu_magic_version.data(),
                               gnu_magic_version.size()) == 0};
    if (!posix && !gnu) {
        return std::unexpected{
            make_error(error_code::parse_error,
                       "rfmod: unsupported tar magic/version (POSIX ustar or "
                       "GNU tar expected)")};
    }

    archive_reader reader;
    if (reader.get() == nullptr) {
        return std::unexpected{
            make_error(error_code::out_of_memory,
                       "rfmod: cannot allocate the libarchive reader")};
    }
    if (archive_read_support_format_tar(reader.get()) != ARCHIVE_OK) {
        return std::unexpected{
            archive_failure(reader.get(), "cannot enable tar format support")};
    }
    if (archive_read_open_memory(reader.get(), tar.data(), tar.size()) !=
        ARCHIVE_OK) {
        return std::unexpected{
            archive_failure(reader.get(), "cannot open the in-memory tar")};
    }

    // Track where the last member's padded payload (or a skipped directory's
    // header) ended. The end-of-archive marker must begin exactly there: a
    // member whose payload is made of NUL bytes can otherwise satisfy the
    // "final 1024 bytes are zero" check after the end blocks were truncated.
    std::uint64_t last_member_end{0};
    std::uint64_t total_payload_bytes{0};
    const auto filter_position =
        [&reader, tar](std::string_view context) -> result<std::uint64_t> {
        const auto position{archive_filter_bytes(reader.get(), -1)};
        if (position < 0 || static_cast<std::uint64_t>(position) > tar.size()) {
            return std::unexpected{make_error(
                error_code::parse_error,
                fmt::format("rfmod: tar reader reported an invalid consumed "
                            "size after {}",
                            context))};
        }
        return static_cast<std::uint64_t>(position);
    };

    archive_entry *entry{nullptr};
    for (;;) {
        const auto next{archive_read_next_header(reader.get(), &entry)};
        if (next == ARCHIVE_EOF) {
            break;
        }
        if (next != ARCHIVE_OK) {
            return std::unexpected{
                archive_failure(reader.get(), "invalid tar archive")};
        }

        const auto filetype{archive_entry_filetype(entry)};
        if (filetype == AE_IFDIR) {
            // Directory entries are optional package metadata: no
            // payload is stored and their absence/presence is not an error.
            // Their names are still attacker-controlled, so they pass through
            // the same path rules as regular members before being skipped.
            const char *raw_directory_name{archive_entry_pathname(entry)};
            const char *directory_name{archive_entry_pathname_utf8(entry)};
            // rfmod paths are UTF-8 by contract. Prefer header bytes that are
            // already valid UTF-8 instead of libarchive's conversion from the
            // ambient locale, which mangles them on non-UTF-8 hosts.
            if (raw_directory_name != nullptr &&
                raw_directory_name[0] != '\0' &&
                data::is_valid_utf8(raw_directory_name)) {
                directory_name = raw_directory_name;
            }
            if (directory_name == nullptr || directory_name[0] == '\0') {
                directory_name = raw_directory_name;
            }
            if (directory_name == nullptr || directory_name[0] == '\0') {
                return std::unexpected{
                    make_error(error_code::parse_error,
                               "rfmod: empty directory member path")};
            }
            std::string directory_path{directory_name};
            // Tar directory names conventionally end in '/', which
            // `is_safe_relative_path` rejects; validate the name without it.
            if (directory_path.back() == '/') {
                directory_path.pop_back();
            }
            if (directory_path.size() > 1024 ||
                !data::is_safe_relative_path(directory_path)) {
                return std::unexpected{make_error(
                    error_code::parse_error,
                    fmt::format("rfmod: unsafe directory member path '{}'",
                                directory_path))};
            }
            if (!data::is_valid_utf8(directory_path)) {
                return std::unexpected{make_error(
                    error_code::parse_error,
                    fmt::format("rfmod: directory member path '{}' is not "
                                "valid UTF-8",
                                directory_path))};
            }
            // Same raw-header check as regular members: libarchive may
            // transcode a non-UTF-8 name from the ambient locale.
            if (raw_directory_name != nullptr &&
                !data::is_valid_utf8(raw_directory_name)) {
                return std::unexpected{make_error(
                    error_code::parse_error,
                    fmt::format("rfmod: directory member path '{}' is not "
                                "valid UTF-8",
                                directory_path))};
            }
            auto position{filter_position("a directory member")};
            if (!position) {
                return std::unexpected{std::move(position.error())};
            }
            last_member_end = *position;
            continue;
        }
        if (archive_entry_hardlink(entry) != nullptr) {
            return std::unexpected{
                make_error(error_code::parse_error,
                           "rfmod: tar hard links are not allowed (only "
                           "regular files)")};
        }
        if (filetype != AE_IFREG) {
            return std::unexpected{make_error(
                error_code::parse_error,
                fmt::format(
                    "rfmod: tar {} entries are not allowed (only "
                    "regular files; directories are skipped)",
                    entry_type_name(static_cast<std::uint32_t>(filetype))))};
        }
        // Sparse members declare a small on-disk payload but expand to their
        // logical size while reading (GNU sparse maps, type 'S'/'x' with
        // sparse data); the packer never emits them, so they are always
        // hostile and must be rejected before their size is trusted.
        if (archive_entry_sparse_count(entry) > 0) {
            return std::unexpected{make_error(
                error_code::parse_error,
                "rfmod: sparse members are not allowed (the packer never emits "
                "them)")};
        }

        // PAX path overrides are already folded into the entry pathname. The
        // UTF-8 accessor is preferred; it falls back to the raw header bytes
        // for archives without an explicit charset.
        const char *raw_name{archive_entry_pathname(entry)};
        const char *name{archive_entry_pathname_utf8(entry)};
        // See the directory branch: keep valid UTF-8 header bytes verbatim.
        if (raw_name != nullptr && raw_name[0] != '\0' &&
            data::is_valid_utf8(raw_name)) {
            name = raw_name;
        }
        if (name == nullptr || name[0] == '\0') {
            name = raw_name;
        }
        if (name == nullptr || name[0] == '\0') {
            return std::unexpected{make_error(error_code::parse_error,
                                              "rfmod: empty member path")};
        }
        std::string path{name};
        if (path.size() > 1024 || !data::is_safe_relative_path(path)) {
            return std::unexpected{
                make_error(error_code::parse_error,
                           "rfmod: unsafe member path '" + path + "'")};
        }
        if (!data::is_valid_utf8(path)) {
            return std::unexpected{
                make_error(error_code::parse_error,
                           "rfmod: member path is not valid UTF-8")};
        }
        // libarchive can transcode a name from the ambient locale charset on
        // some hosts; the bytes actually stored in the archive must be UTF-8
        // too, otherwise a non-UTF-8 name could be normalised past the check.
        if (raw_name != nullptr && !data::is_valid_utf8(raw_name)) {
            return std::unexpected{
                make_error(error_code::parse_error,
                           "rfmod: member path is not valid UTF-8")};
        }
        if (index.contains(path)) {
            return std::unexpected{
                make_error(error_code::parse_error,
                           "rfmod: duplicate member path '" + path + "'")};
        }
        if (entries.size() >= options.max_files) {
            return std::unexpected{
                make_error(error_code::parse_error,
                           "rfmod: member count exceeds the configured limit")};
        }

        const auto declared{archive_entry_size(entry)};
        if (!archive_entry_size_is_set(entry) || declared < 0) {
            return std::unexpected{
                make_error(error_code::parse_error,
                           "rfmod: member '" + path + "' has no valid size")};
        }
        const auto declared_size{static_cast<std::uint64_t>(declared)};
        if (declared_size > options.max_file_bytes) {
            return std::unexpected{
                make_error(error_code::parse_error,
                           "rfmod: member '" + path +
                               "' exceeds the configured size limit")};
        }
        // The decompressed tar and its member payloads are both bounded by
        // `max_archive_bytes`, so one hostile header cannot stack many members
        // whose declared sizes add up to more than the archive budget. This
        // check runs first so a small `max_archive_bytes` is reported as a
        // budget violation even when the size also exceeds the tar framing.
        const auto archive_budget{
            static_cast<std::uint64_t>(options.max_archive_bytes)};
        if (total_payload_bytes > archive_budget ||
            declared_size > archive_budget - total_payload_bytes) {
            return std::unexpected{make_error(
                error_code::parse_error,
                "rfmod: members exceed the decompressed archive budget")};
        }
        // A member's declared payload must fit in the bytes left in the tar
        // stream. This stops sparse variants whose map has no stored extent
        // (so `archive_entry_sparse_count` is 0) but whose logical size is
        // huge: libarchive would synthesise the holes from the size alone.
        auto header_end{filter_position("member '" + path + "' header")};
        if (!header_end) {
            return std::unexpected{std::move(header_end.error())};
        }
        const auto remaining_archive_bytes{
            static_cast<std::uint64_t>(tar.size()) - *header_end};
        if (declared_size > remaining_archive_bytes) {
            return std::unexpected{make_error(
                error_code::parse_error,
                "rfmod: member '" + path +
                    "' declares more data than the remaining archive bytes")};
        }

        std::vector<std::byte> payload;
        payload.reserve(static_cast<std::size_t>(
            std::min<std::uint64_t>(declared_size, 1024U * 1024U)));
        std::array<std::byte, 64U * 1024U> chunk{};
        std::uint64_t total{0};
        for (;;) {
            const auto read{
                archive_read_data(reader.get(), chunk.data(), chunk.size())};
            if (read < 0) {
                return std::unexpected{archive_failure(
                    reader.get(), "cannot read member '" + path + "'")};
            }
            if (read == 0) {
                break;
            }
            const auto chunk_size{static_cast<std::uint64_t>(read)};
            if (chunk_size > declared_size - total) {
                return std::unexpected{
                    make_error(error_code::parse_error,
                               "rfmod: member '" + path +
                                   "' contains more data than its declared "
                                   "size")};
            }
            total += chunk_size;
            payload.insert(payload.end(), chunk.begin(),
                           chunk.begin() +
                               static_cast<std::ptrdiff_t>(chunk_size));
        }
        if (total != declared_size) {
            return std::unexpected{
                make_error(error_code::parse_error,
                           "rfmod: truncated member data for '" + path + "'")};
        }

        auto position{filter_position("member '" + path + "'")};
        if (!position) {
            return std::unexpected{std::move(position.error())};
        }
        last_member_end = *position;
        total_payload_bytes += declared_size;

        index.emplace(path, entries.size());
        entries.push_back(
            rfmod_entry{.path = std::move(path), .size = declared_size});
        payloads.push_back(std::move(payload));
    }

    // The two zero blocks must begin exactly where the last member's padded
    // payload ended; checking only the final 1024 bytes would accept a
    // truncated archive whose member data happens to end in zero bytes.
    // Trailing zero padding after the blocks is still fine.
    const auto end_offset{static_cast<std::size_t>(last_member_end)};
    if (tar.size() - end_offset < tar_end_blocks * tar_header_size ||
        !all_zero(tar.subspan(end_offset))) {
        return std::unexpected{
            make_error(error_code::parse_error,
                       "rfmod: missing end-of-archive blocks after the last "
                       "member (truncated tar)")};
    }

    // libarchive stops at the first end-of-archive marker. Everything it did
    // not consume must be zero padding: otherwise a second tar/garbage block
    // could hide after the end blocks (the final-block check above only looks
    // at the very end of the stream).
    const auto consumed{archive_filter_bytes(reader.get(), -1)};
    if (consumed < 0 || static_cast<std::uint64_t>(consumed) > tar.size()) {
        return std::unexpected{
            make_error(error_code::parse_error,
                       "rfmod: tar reader reported an invalid consumed size")};
    }
    if (!all_zero(tar.subspan(static_cast<std::size_t>(consumed)))) {
        return std::unexpected{
            make_error(error_code::parse_error,
                       "rfmod: trailing data after the end-of-archive blocks")};
    }
    return status_ok();
}

rfmod_reader::rfmod_reader() : impl_{std::make_unique<impl>()} {}

rfmod_reader::rfmod_reader(rfmod_reader &&other) noexcept = default;

auto rfmod_reader::operator=(rfmod_reader &&other) noexcept
    -> rfmod_reader & = default;

rfmod_reader::~rfmod_reader() = default;

auto rfmod_reader::open(std::span<const std::byte> archive,
                        rfmod_read_options options) -> result<rfmod_reader> {
    if (archive.empty()) {
        return std::unexpected{
            make_error(error_code::parse_error, "rfmod: empty archive")};
    }
    auto tar{decompress_zstd(archive, options.max_archive_bytes)};
    if (!tar) {
        return std::unexpected{std::move(tar.error())};
    }
    rfmod_reader reader;
    reader.impl_->archive_digest = data::blake3(archive);
    reader.impl_->decompressed_size = tar->size();
    if (auto parsed =
            reader.impl_->parse(std::span<const std::byte>{*tar}, options);
        !parsed) {
        return std::unexpected{std::move(parsed.error())};
    }
    return reader;
}

auto rfmod_reader::open_file(std::string_view path, rfmod_read_options options)
    -> result<rfmod_reader> {
    // Bound the compressed container before reading it, so a sparse or huge
    // file cannot make `read_file` allocate its full size first (DoS).
    auto size{io::file_size(path)};
    if (!size) {
        return std::unexpected{std::move(size.error())};
    }
    if (*size > options.max_container_bytes) {
        return std::unexpected{make_error(
            error_code::invalid_argument,
            fmt::format("rfmod: container '{}' is {} bytes, exceeding the "
                        "configured limit of {} bytes",
                        path, *size, options.max_container_bytes))};
    }
    auto bytes{io::read_file(path)};
    if (!bytes) {
        return std::unexpected{std::move(bytes.error())};
    }
    // Re-check after reading: the file may have grown between `file_size` and
    // `read_file` (replacement/append race).
    if (bytes->size() > options.max_container_bytes) {
        return std::unexpected{make_error(
            error_code::invalid_argument,
            fmt::format("rfmod: container '{}' grew to {} bytes while "
                        "reading, exceeding the configured limit of {} bytes",
                        path, bytes->size(), options.max_container_bytes))};
    }
    // Qualified on purpose: keeps the platform guard's bare-OS-call heuristic
    // from mistaking this static factory call for the POSIX `::open` syscall.
    return rfmod_reader::open(std::span<const std::byte>{*bytes}, options);
}

auto rfmod_reader::entries() const noexcept
    -> const std::vector<rfmod_entry> & {
    return impl_->entries;
}

auto rfmod_reader::find(std::string_view path) const noexcept
    -> const rfmod_entry * {
    const auto found{impl_->index.find(path)};
    return found == impl_->index.end() ? nullptr
                                       : &impl_->entries[found->second];
}

auto rfmod_reader::read(std::string_view path) const
    -> result<std::vector<std::byte>> {
    const auto found{impl_->index.find(path)};
    if (found == impl_->index.end()) {
        return std::unexpected{
            make_error(error_code::not_found,
                       "rfmod: no member '" + std::string{path} + "'")};
    }
    return std::vector<std::byte>{impl_->payloads[found->second]};
}

auto rfmod_reader::archive_digest() const noexcept -> const data::digest32 & {
    return impl_->archive_digest;
}

auto rfmod_reader::decompressed_size() const noexcept -> std::size_t {
    return impl_->decompressed_size;
}

auto rfmod_reader::file_count() const noexcept -> std::size_t {
    return impl_->entries.size();
}

// ---------------------------------------------------------------------------
// verify
// ---------------------------------------------------------------------------

auto verify_rfmod(const rfmod_reader &reader,
                  const rfmod_verify_options &options)
    -> result<rfmod_verification> {
    if (reader.find(rfmod_manifest_path) == nullptr) {
        return std::unexpected{
            make_error(error_code::not_found,
                       "rfmod verify: archive has no root manifest.toml")};
    }
    auto manifest_bytes{reader.read(rfmod_manifest_path)};
    if (!manifest_bytes) {
        return std::unexpected{std::move(manifest_bytes.error())};
    }
    const std::string manifest_text{
        reinterpret_cast<const char *>(manifest_bytes->data()), // NOLINT
        manifest_bytes->size()};
    auto package{parse_package_manifest(manifest_text, rfmod_manifest_path)};
    if (!package) {
        return std::unexpected{
            annotate(std::move(package.error()), "rfmod verify")};
    }
    // Per-file integrity: every member must match its [files] record and every
    // record must have a member (checked before the signature so a tampered
    // container is reported as such).
    if (auto covered{check_coverage(reader, package->files)}; !covered) {
        return std::unexpected{
            annotate(std::move(covered.error()), "rfmod verify")};
    }

    rfmod_verification result;
    result.archive_digest = reader.archive_digest();
    result.manifest_digest = data::blake3(as_bytes(package->text));
    if (auto policy{apply_signature_policy(*package, options, result)};
        !policy) {
        return std::unexpected{
            annotate(std::move(policy.error()), "rfmod verify")};
    }
    result.manifest = std::move(package->manifest);
    result.files = std::move(package->files);
    return result;
}

// ---------------------------------------------------------------------------
// install
// ---------------------------------------------------------------------------

namespace {

/// True when `component` is safe as a single path segment below
/// `<store>/mods/`. Manifest ids and SemVer text are grammar-validated
/// already; this is the defensive check for path construction (a parsed field
/// is never trusted as a file name). Only the values that could escape or
/// confuse the path are rejected: empty, "." and ".." exactly, separators and
/// control bytes. Ordinary double dots inside a name (`a..b` is a valid mod
/// id) are allowed.
[[nodiscard]] auto is_safe_receipt_segment(std::string_view component) noexcept
    -> bool {
    if (component.empty() || component == "." || component == "..") {
        return false;
    }
    return std::none_of(component.begin(), component.end(), [](char character) {
        const auto byte{static_cast<unsigned char>(character)};
        return character == '/' || character == '\\' || byte < 0x20U ||
               byte == 0x7FU;
    });
}

/// `<store>/mods/<id>/<version>.json`, or `invalid_argument` when either
/// component could escape its directory.
[[nodiscard]] auto receipt_path_for(const data::content_store &store,
                                    const mod::mod_manifest &manifest)
    -> result<std::string> {
    const auto &common{manifest.common};
    if (!is_safe_receipt_segment(common.id) ||
        !is_safe_receipt_segment(common.version)) {
        return std::unexpected{make_error(
            error_code::invalid_argument,
            fmt::format("rfmod install: id '{}' or version '{}' is not a safe "
                        "single path segment for the install receipt",
                        common.id, common.version))};
    }
    return fmt::format("{}/mods/{}/{}.json", store.root(), common.id,
                       common.version);
}

/// `<store>/mods/pins/<id>.json`.
[[nodiscard]] auto signer_pin_path(std::string_view store_root,
                                   std::string_view id) -> result<std::string> {
    if (!is_safe_receipt_segment(id)) {
        return std::unexpected{make_error(
            error_code::invalid_argument,
            fmt::format("mod signer pin: '{}' is not a safe single path "
                        "segment",
                        id))};
    }
    return fmt::format("{}/mods/pins/{}.json", store_root, id);
}

/// Residency of the receipt's `policy` field. A signed install is accepted
/// under the continuity policy in force; an unsigned one records `unsigned`.
[[nodiscard]] auto receipt_policy_text(bool signed_package,
                                       rfmod_signature_policy policy)
    -> std::string {
    if (!signed_package) {
        return "unsigned";
    }
    return std::string{mod::to_string(policy)};
}

/// Current UTC time as `YYYY-MM-DDTHH:MM:SSZ`.
[[nodiscard]] auto utc_now_text() -> std::string {
    const auto now{std::chrono::system_clock::now()};
    return fmt::format("{:%Y-%m-%dT%H:%M:%SZ}",
                       fmt::gmtime(std::chrono::system_clock::to_time_t(now)));
}

/// Serialize the install receipt (schema `refrain.mod-receipt/1`).
///
/// `json::dump()` writes object members in sorted key order and the file list
/// is sorted by path bytes, so the same install with the same `installed_at`
/// produces byte-identical receipts. `installed_at` and `installer_version` are
/// the only non-content fields; both are supplied by the caller.
[[nodiscard]] auto format_install_receipt(const rfmod_install_result &result,
                                          std::string_view installed_at,
                                          std::string_view installer_version)
    -> std::string {
    using json_value = data::json::value;

    std::vector<const installed_file *> ordered;
    ordered.reserve(result.files.size());
    for (const auto &file : result.files) {
        ordered.push_back(&file);
    }
    std::sort(ordered.begin(), ordered.end(),
              [](const installed_file *lhs, const installed_file *rhs) {
                  return lhs->path < rhs->path;
              });

    json_value::array_type files;
    files.reserve(ordered.size());
    for (const auto *file : ordered) {
        json_value::object_type entry;
        entry.emplace("path", json_value{file->path});
        // The tar size field bounds every member far below INT64_MAX; this is
        // the DOM's integer representation, not a truncation.
        entry.emplace("size",
                      json_value{static_cast<std::int64_t>(file->size)});
        entry.emplace("blake3", json_value{data::to_hex(file->digest)});
        entry.emplace("mode", json_value{file->mode});
        files.emplace_back(std::move(entry));
    }

    json_value::object_type receipt;
    receipt.emplace("schema", json_value{std::string{mod_receipt_schema}});
    receipt.emplace("id", json_value{result.manifest.common.id});
    receipt.emplace("version", json_value{result.manifest.common.version});
    receipt.emplace("abi", json_value{result.manifest.common.abi});
    receipt.emplace("archive_blake3",
                    json_value{data::to_hex(result.archive_digest)});
    receipt.emplace("manifest_blake3",
                    json_value{data::to_hex(result.manifest_digest)});
    receipt.emplace("signed", json_value{result.signed_package});
    receipt.emplace("trusted", json_value{result.trusted});
    receipt.emplace("key_id", json_value{result.key_id});
    receipt.emplace("public_key_fingerprint",
                    json_value{result.public_key_fingerprint});
    receipt.emplace("policy", json_value{result.receipt_policy});
    receipt.emplace("installed_at", json_value{std::string{installed_at}});
    receipt.emplace("installer_version",
                    json_value{std::string{installer_version}});
    receipt.emplace("origin",
                    json_value{std::string{mod::to_string(result.origin)}});
    receipt.emplace("files", json_value{std::move(files)});
    return json_value{std::move(receipt)}.dump();
}

/// Serialize a signer pin (schema `refrain.mod-signer-pin/1`).
[[nodiscard]] auto format_signer_pin(const mod_signer_pin &pin) -> std::string {
    using json_value = data::json::value;
    json_value::object_type document;
    document.emplace("schema", json_value{std::string{mod_signer_pin_schema}});
    document.emplace("id", json_value{pin.id});
    document.emplace("key_id", json_value{pin.key_id});
    document.emplace("public_key_fingerprint",
                     json_value{pin.public_key_fingerprint});
    document.emplace("policy",
                     json_value{std::string{mod::to_string(pin.policy)}});
    document.emplace("pinned_at", json_value{pin.pinned_at});
    document.emplace("source", json_value{pin.source});
    return json_value{std::move(document)}.dump();
}

/// Soft target size of the single content segment batch opened by one install.
constexpr std::uint32_t install_segment_target_bytes{32U * 1024U * 1024U};

/// RFBL packed block size. Packed members record only
/// their logical size, so the RFBT `block_count` is the deterministic
/// `ceil(logical_size / 65536)` of the packed writer.
constexpr std::uint64_t packed_blob_block_bytes{65536};

/// `<store>/content/blobs.idx` (the global blob table).
[[nodiscard]] auto blob_table_path(const data::content_store &store)
    -> std::string {
    return fmt::format("{}/content/{}", store.root(),
                       data::content_blob_table_filename);
}

/// Parse the existing global blob table when the store has one.
///
/// The table is read before the first blob write: a damaged or foreign-version
/// `blobs.idx` fails the install instead of being silently replaced. A missing
/// file is `nullopt` (a store that never published a table).
[[nodiscard]] auto load_install_blob_table(const data::content_store &store)
    -> result<std::optional<data::blob_table_view>> {
    const auto path{blob_table_path(store)};
    if (!io::exists(path)) {
        return std::optional<data::blob_table_view>{};
    }
    auto bytes{io::read_file(path)};
    if (!bytes) {
        return std::unexpected{
            annotate(std::move(bytes.error()), "rfmod install: blobs.idx")};
    }
    auto parsed{data::parse_blob_table(*bytes)};
    if (!parsed) {
        return std::unexpected{
            annotate(std::move(parsed.error()), "rfmod install: blobs.idx")};
    }
    return std::optional<data::blob_table_view>{std::move(*parsed)};
}

/// Merge the batch's new records into the existing table.
///
/// Existing records are copied verbatim (kind/flags/refcount/location) because
/// install never owns them; new records use `kind = content` and `refcount = 0`
/// until a resolution index or another root claims the blob.
[[nodiscard]] auto
build_install_blob_table(const std::optional<data::blob_table_view> &existing,
                         std::span<const data::blob_metadata> created)
    -> result<std::vector<std::byte>> {
    std::vector<data::blob_table_entry> entries;
    if (existing.has_value()) {
        entries.assign(existing->entries.begin(), existing->entries.end());
    }
    entries.reserve(entries.size() + created.size());
    for (const auto &meta : created) {
        data::blob_table_entry entry{};
        entry.digest = meta.digest;
        entry.logical_size = meta.logical_size;
        entry.flags = meta.flags;
        entry.refcount = 0;
        entry.kind = data::blob_kind_content;
        entry.stored_size = meta.stored_size;
        entry.segment_id = meta.segment_id;
        entry.segment_offset = meta.segment_offset;
        if ((meta.flags & data::blob_flags_packed) != 0U) {
            const auto block_count{
                (meta.logical_size + packed_blob_block_bytes - 1U) /
                packed_blob_block_bytes};
            if (block_count > std::numeric_limits<std::uint32_t>::max()) {
                return std::unexpected{
                    annotate(make_error(error_code::invalid_argument,
                                        "packed blob block count exceeds the "
                                        "32-bit RFBT field"),
                             "rfmod install: blobs.idx")};
            }
            entry.block_count = static_cast<std::uint32_t>(block_count);
        }
        entries.push_back(entry);
    }

    auto bytes{data::build_blob_table(entries)};
    if (!bytes) {
        return std::unexpected{
            annotate(std::move(bytes.error()), "rfmod install: blobs.idx")};
    }
    return bytes;
}

/// One existing resolution index loaded against the previous blob table.
struct install_resolution_index {
    std::string path{};
    data::resolution_index_view view{};
};

/// Sorted paths of `<store>/content/resolutions/*.idx` (empty when absent).
[[nodiscard]] auto install_resolution_paths(const data::content_store &store)
    -> result<std::vector<std::string>> {
    const auto directory{
        data::join_path(store.root(), data::content_resolutions_directory)};
    std::vector<std::string> paths;
    if (!io::is_directory(directory)) {
        return paths;
    }
    auto names{io::list_directory(directory)};
    if (!names) {
        return std::unexpected{
            annotate(std::move(names.error()), "rfmod install: resolutions")};
    }
    for (const auto &name : *names) {
        if (name.size() <= 4U ||
            name.compare(name.size() - 4U, 4U, ".idx") != 0) {
            continue;
        }
        paths.push_back(data::join_path(directory, name));
    }
    std::sort(paths.begin(), paths.end());
    return paths;
}

/// Load every published resolution index against `table`.
///
/// An index binds itself to the BLAKE3 prefix of `blobs.idx`, so it can only be
/// parsed against the table it was published with; an index with no table is a
/// damaged store and fails before the first write.
[[nodiscard]] auto load_install_resolution_indexes(
    const data::content_store &store,
    const std::optional<data::blob_table_view> &table)
    -> result<std::vector<install_resolution_index>> {
    auto paths{install_resolution_paths(store)};
    if (!paths) {
        return std::unexpected{std::move(paths.error())};
    }
    std::vector<install_resolution_index> indexes;
    indexes.reserve(paths->size());
    for (const auto &path : *paths) {
        if (!table.has_value()) {
            return std::unexpected{
                make_error(error_code::invalid_argument,
                           "rfmod install: existing resolution index '" + path +
                               "' cannot be read without blobs.idx")};
        }
        auto bytes{io::read_file(path)};
        if (!bytes) {
            return std::unexpected{
                annotate(std::move(bytes.error()), "rfmod install: " + path)};
        }
        auto parsed{data::parse_resolution_index(*bytes, *table)};
        if (!parsed) {
            return std::unexpected{
                annotate(std::move(parsed.error()), "rfmod install: " + path)};
        }
        indexes.push_back(
            install_resolution_index{.path = path, .view = std::move(*parsed)});
    }
    return indexes;
}

struct rebuilt_resolution_index {
    std::string path{};
    std::vector<std::byte> bytes{};
};

/// Re-encode every existing resolution index against the merged table.
///
/// Publishing a changed `blobs.idx` changes its BLAKE3 prefix, which every RFRX
/// binds; the `blob_index` positions can also shift because the table is
/// digest-sorted. Both are fixed by re-reading each entry's digest from the
/// previous table and rebuilding the index against the new one (the same
/// transaction `content build` performs).
[[nodiscard]] auto rebuild_install_resolution_indexes(
    std::span<const install_resolution_index> indexes,
    const data::blob_table_view &previous, const data::blob_table_view &next)
    -> result<std::vector<rebuilt_resolution_index>> {
    std::vector<rebuilt_resolution_index> rebuilt;
    rebuilt.reserve(indexes.size());
    for (const auto &index : indexes) {
        std::vector<data::resolution_input> inputs;
        inputs.reserve(index.view.entries.size());
        for (const auto &entry : index.view.entries) {
            inputs.push_back(data::resolution_input{
                .location = entry.location,
                .source = entry.source,
                .priority = entry.priority,
                .blob = previous.entries[entry.blob_index].digest,
                .flags = 0,
            });
        }
        auto bytes{data::build_resolution_index(inputs, next)};
        if (!bytes) {
            return std::unexpected{annotate(std::move(bytes.error()),
                                            "rfmod install: " + index.path)};
        }
        rebuilt.push_back(rebuilt_resolution_index{.path = index.path,
                                                   .bytes = std::move(*bytes)});
    }
    return rebuilt;
}

// ---------------------------------------------------------------------------
// update policy (signature continuity, version monotonicity)
// ---------------------------------------------------------------------------

/// Effective signer state of one mod id before an install.
enum class signer_state : std::uint8_t {
    none,   ///< no installed version and no signer pin: first install
    pinned, ///< a signer is pinned (pin file or a signed receipt)
};

/// Everything an update decision needs about the current install of one id.
struct installed_state {
    std::vector<mod_receipt> receipts{};
    std::optional<mod_signer_pin> pin{};
    signer_state state{signer_state::none};
};

/// Highest installed version (SemVer precedence; equal precedence is broken by
/// the raw version text so the choice is deterministic). `nullptr` when empty.
[[nodiscard]] auto latest_receipt(std::span<const mod_receipt> receipts)
    -> const mod_receipt * {
    const mod_receipt *best{nullptr};
    core::semver best_version{};
    for (const auto &receipt : receipts) {
        auto version{core::semver::parse(receipt.version)};
        if (!version) {
            continue;
        }
        if (best == nullptr) {
            best = &receipt;
            best_version = std::move(*version);
            continue;
        }
        const auto order{core::compare(*version, best_version)};
        if (order == std::strong_ordering::greater ||
            (order == std::strong_ordering::equal &&
             receipt.version > best->version)) {
            best = &receipt;
            best_version = std::move(*version);
        }
    }
    return best;
}

/// Load the installed receipts of `id` and resolve the effective signer pin.
///
/// The pin file is authoritative. Without one, a signed receipt pins its
/// signer (key id plus fingerprint when the key bytes were configured).
[[nodiscard]] auto load_installed_state(std::string_view store_root,
                                        std::string_view id)
    -> result<installed_state> {
    auto paths{list_receipt_paths_for(store_root, id)};
    if (!paths) {
        return std::unexpected{std::move(paths.error())};
    }
    installed_state state;
    state.receipts.reserve(paths->size());
    for (const auto &path : *paths) {
        auto receipt{read_receipt(path)};
        if (!receipt) {
            return std::unexpected{std::move(receipt.error())};
        }
        if (receipt->id != id) {
            return std::unexpected{make_error(
                error_code::parse_error,
                fmt::format("mod install: receipt '{}' records id '{}' but "
                            "lives under '{}'",
                            path, receipt->id, id))};
        }
        state.receipts.push_back(std::move(*receipt));
    }

    auto file_pin{read_signer_pin(store_root, id)};
    if (!file_pin) {
        return std::unexpected{std::move(file_pin.error())};
    }
    if (*file_pin) {
        state.pin = std::move(**file_pin);
        state.state = signer_state::pinned;
        return state;
    }

    if (const auto *latest{latest_receipt(state.receipts)};
        latest != nullptr && latest->pinned()) {
        state.pin = mod_signer_pin{
            .id = std::string{id},
            .key_id = latest->key_id,
            .public_key_fingerprint = latest->public_key_fingerprint,
            .policy = parse_rfmod_signature_policy(latest->policy)
                          .value_or(rfmod_signature_policy::strict),
            .pinned_at = {},
            .source = "receipt"};
        state.state = signer_state::pinned;
    }
    return state;
}

/// New signer identity carried by the package being installed.
struct package_signer {
    bool signed_package{false};
    std::string key_id{};
    std::string fingerprint{};
};

/// Result of the pre-write update-policy check.
struct update_decision {
    /// Exact archive digest already installed: nothing to do.
    bool already_installed{false};
    /// Existing receipt path of the idempotent archive (when
    /// `already_installed`).
    std::string existing_receipt{};
    /// Installed members of the idempotent archive (when `already_installed`).
    std::vector<receipt_file> existing_files{};
    /// Pin to publish after a successful install (`nullopt` = keep the current
    /// state and write nothing).
    std::optional<mod_signer_pin> next_pin{};
    bool downgrade_override{false};
    bool same_version_override{false};
    bool signer_rotated{false};
};

/// Apply the Android-style update rules for one package.
///
/// Returns an error and writes nothing when the update is refused. The checks,
/// in order: version monotonicity, same-version content identity, signer
/// continuity (stored signer vs package signer, rotation allowlist, signed to
/// unsigned).
[[nodiscard]] auto
decide_update(std::string_view store_root, std::string_view id,
              std::string_view version, const data::digest32 &archive_digest,
              const package_signer &signer, const installed_state &state,
              const rfmod_install_options &options) -> result<update_decision> {
    update_decision decision;

    // Exact (id, version) receipt: the archive digest is the content identity.
    // The receipt file name is the version text, so an exact match is also the
    // slot the install would overwrite.
    const auto same_version{std::find_if(state.receipts.begin(),
                                         state.receipts.end(),
                                         [version](const mod_receipt &receipt) {
                                             return receipt.version == version;
                                         })};
    if (same_version != state.receipts.end() &&
        same_version->archive_blake3 == archive_digest) {
        decision.already_installed = true;
        decision.existing_receipt =
            fmt::format("{}/mods/{}/{}.json", store_root, id, version);
        decision.existing_files = same_version->files;
        return decision;
    }

    bool lower_than_installed{false};
    if (const auto *latest{latest_receipt(state.receipts)}; latest != nullptr) {
        auto incoming{core::semver::parse(version)};
        auto installed{core::semver::parse(latest->version)};
        if (incoming && installed) {
            lower_than_installed = core::compare(*incoming, *installed) ==
                                   std::strong_ordering::less;
        }
    }
    if (lower_than_installed) {
        if (!options.allow_downgrade) {
            const auto *latest{latest_receipt(state.receipts)};
            return std::unexpected{make_error(
                error_code::version_mismatch,
                fmt::format("mod install: '{}' version {} is lower than the "
                            "installed {}; pass --allow-downgrade to accept a "
                            "downgrade",
                            id, version,
                            latest == nullptr ? std::string{"?"}
                                              : latest->version))};
        }
        decision.downgrade_override = true;
    }
    if (same_version != state.receipts.end()) {
        if (!options.replace_same_version) {
            return std::unexpected{make_error(
                error_code::already_exists,
                fmt::format("mod install: '{}' version {} is already installed "
                            "with a different archive digest; bump the version "
                            "or pass --replace-same-version",
                            id, version))};
        }
        decision.same_version_override = true;
    }

    const auto make_pin = [&](std::string source,
                              std::string fingerprint) -> mod_signer_pin {
        return mod_signer_pin{.id = std::string{id},
                              .key_id = signer.key_id,
                              .public_key_fingerprint = std::move(fingerprint),
                              .policy = options.signature_policy,
                              .pinned_at = utc_now_text(),
                              .source = std::move(source)};
    };

    switch (state.state) {
    case signer_state::none:
        if (signer.signed_package) {
            decision.next_pin = make_pin("install", signer.fingerprint);
        }
        return decision;

    case signer_state::pinned:
        break;
    }

    const auto &pin{*state.pin};
    if (!signer.signed_package) {
        return std::unexpected{make_error(
            error_code::permission_denied,
            fmt::format("mod install: '{}' is pinned to signer '{}' but the "
                        "update is unsigned; signed-to-unsigned updates are "
                        "rejected",
                        id, pin.key_id))};
    }
    if (signer.key_id == pin.key_id) {
        if (!pin.public_key_fingerprint.empty() &&
            !signer.fingerprint.empty() &&
            signer.fingerprint != pin.public_key_fingerprint) {
            return std::unexpected{make_error(
                error_code::permission_denied,
                fmt::format("mod install: '{}' update is signed by key id '{}' "
                            "but a different key than the pinned fingerprint",
                            id, signer.key_id))};
        }
        decision.next_pin = make_pin("install", signer.fingerprint.empty()
                                                    ? pin.public_key_fingerprint
                                                    : signer.fingerprint);
        return decision;
    }

    const auto rotation{std::find_if(
        options.rotations.begin(), options.rotations.end(),
        [&](const rfmod_key_rotation &entry) {
            return entry.id == id && entry.old_key_id == pin.key_id &&
                   entry.new_key_id == signer.key_id;
        })};
    if (rotation != options.rotations.end()) {
        decision.signer_rotated = true;
        decision.next_pin = make_pin("rotation", signer.fingerprint);
        return decision;
    }
    if (options.signature_policy == rfmod_signature_policy::trusted_any) {
        decision.signer_rotated = true;
        decision.next_pin = make_pin("trusted-any", signer.fingerprint);
        return decision;
    }
    return std::unexpected{make_error(
        error_code::permission_denied,
        fmt::format("mod install: '{}' is pinned to signer '{}' but the update "
                    "is signed by '{}'; add a rotation entry for {} -> {} to "
                    "allow the change",
                    id, pin.key_id, signer.key_id, pin.key_id, signer.key_id))};
}

// ---------------------------------------------------------------------------
// staging
// ---------------------------------------------------------------------------

/// `<store>/content/.staging` (hidden: content-store transaction debris).
[[nodiscard]] auto staging_root(std::string_view store_root) -> std::string {
    return fmt::format("{}/content/.staging", store_root);
}

/// True when `entry` is a staging directory name (all staging dirs are
/// prefixed so accidental entries are not deleted).
[[nodiscard]] auto is_staging_entry(std::string_view name) noexcept -> bool {
    return name.size() > 0 && name.front() != '.';
}

/// Remove one directory tree, ignoring "already gone" races.
void remove_staging_tree(const std::string &path) noexcept {
    std::error_code error;
    std::filesystem::remove_all(std::filesystem::path{path}, error);
}

/// Write `<staging>/owner` (`pid=<n>` / `started=<unix>`) so a later install
/// can tell a crashed staging directory from one a live process is using.
[[nodiscard]] auto write_staging_owner(const std::string &directory) -> status {
    const auto now{std::chrono::duration_cast<std::chrono::seconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                       .count()};
    return io::atomic_write_text(
        fmt::format("{}/owner", directory),
        fmt::format("pid={}\nstarted={}\n", platform::process_id(), now));
}

/// Parse a staging owner record; `nullopt` when it is missing or malformed.
struct staging_owner {
    std::uint64_t pid{0};
    std::int64_t started{0};
};

[[nodiscard]] auto read_staging_owner(const std::string &directory)
    -> std::optional<staging_owner> {
    auto text{io::read_text(fmt::format("{}/owner", directory),
                            {.max_bytes = 4096, .allow_symlink = false})};
    if (!text) {
        return std::nullopt;
    }
    staging_owner owner;
    bool saw_pid{false};
    bool saw_started{false};
    std::size_t offset{0};
    while (offset < text->size()) {
        auto end{text->find('\n', offset)};
        if (end == std::string::npos) {
            end = text->size();
        }
        const std::string_view line{text->data() + offset, end - offset};
        offset = end + 1U;
        if (line.starts_with("pid=")) {
            std::uint64_t value{0};
            const auto [pointer, error]{std::from_chars(
                line.data() + 4, line.data() + line.size(), value)};
            if (error != std::errc{} || pointer != line.data() + line.size()) {
                return std::nullopt;
            }
            owner.pid = value;
            saw_pid = true;
        } else if (line.starts_with("started=")) {
            std::int64_t value{0};
            const auto [pointer, error]{std::from_chars(
                line.data() + 8, line.data() + line.size(), value)};
            if (error != std::errc{} || pointer != line.data() + line.size()) {
                return std::nullopt;
            }
            owner.started = value;
            saw_started = true;
        }
    }
    if (!saw_pid || !saw_started) {
        return std::nullopt;
    }
    return owner;
}

/// Remove staging directories left behind by a crashed install.
///
/// A directory whose owner process is still alive (and whose record is younger
/// than a day) is left alone; everything else is a crash leftover. Callers hold
/// the store lock, so no live installer can be between creating its directory
/// and writing the owner record.
[[nodiscard]] auto prune_staging(const std::string &root, std::size_t &removed)
    -> status {
    if (!io::is_directory(root)) {
        return status_ok();
    }
    auto names{io::list_directory(root)};
    if (!names) {
        return std::unexpected{
            annotate(std::move(names.error()), "rfmod install: staging")};
    }
    const auto now{std::chrono::duration_cast<std::chrono::seconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                       .count()};
    for (const auto &name : *names) {
        if (!is_staging_entry(name)) {
            continue;
        }
        const auto directory{data::join_path(root, name)};
        if (!io::is_directory(directory)) {
            continue;
        }
        bool stale{true};
        if (const auto owner{read_staging_owner(directory)}; owner) {
            constexpr std::int64_t owner_max_age_seconds{24 * 60 * 60};
            const auto fresh{owner->started > 0 &&
                             now - owner->started < owner_max_age_seconds};
            stale = !(fresh && platform::process_alive(owner->pid));
        }
        if (!stale) {
            continue;
        }
        remove_staging_tree(directory);
        ++removed;
    }
    return status_ok();
}

/// RAII cleanup of one staging directory.
class staging_guard {
  public:
    explicit staging_guard(std::string path) : path_{std::move(path)} {}
    staging_guard(const staging_guard &) = delete;
    auto operator=(const staging_guard &) -> staging_guard & = delete;
    staging_guard(staging_guard &&other) noexcept
        : path_{std::move(other.path_)} {
        other.path_.clear();
    }
    auto operator=(staging_guard &&other) noexcept -> staging_guard & {
        if (this != &other) {
            cleanup();
            path_ = std::move(other.path_);
            other.path_.clear();
        }
        return *this;
    }
    ~staging_guard() { cleanup(); }

    [[nodiscard]] auto path() const noexcept -> const std::string & {
        return path_;
    }
    /// Keep the directory after the scope ends (used only by tests via
    /// `rfmod_install_options::keep_staging`).
    void release() noexcept { path_.clear(); }

  private:
    void cleanup() noexcept {
        if (!path_.empty()) {
            remove_staging_tree(path_);
            path_.clear();
        }
    }

    std::string path_{};
};

/// Create one unique staging directory below `<store>/mods/.staging`.
[[nodiscard]] auto create_staging_directory(const data::content_store &store,
                                            std::string_view id,
                                            std::string_view version)
    -> result<std::string> {
    const auto root{staging_root(store.root())};
    if (auto created{io::create_directories(root)}; !created) {
        return std::unexpected{
            annotate(std::move(created.error()), "rfmod install: staging")};
    }
    const auto base{
        fmt::format("{}/{}-{}-{}", root, id, version, platform::process_id())};
    for (std::size_t attempt{0}; attempt < 1000U; ++attempt) {
        const auto candidate{
            attempt == 0U ? base : fmt::format("{}-{}", base, attempt)};
        std::error_code error;
        if (std::filesystem::create_directory(std::filesystem::path{candidate},
                                              error)) {
            return candidate;
        }
        if (error && error != std::errc::file_exists) {
            return std::unexpected{make_error(
                error_code::io_error,
                fmt::format("rfmod install: cannot create the staging "
                            "directory '{}': {}",
                            candidate, error.message()))};
        }
    }
    return std::unexpected{make_error(
        error_code::io_error,
        fmt::format("rfmod install: cannot find a free staging directory "
                    "below '{}'",
                    root))};
}

} // namespace

auto install_rfmod(const rfmod_reader &reader, data::content_store &store,
                   const rfmod_install_options &options)
    -> result<rfmod_install_result> {
    if (!store.valid()) {
        return std::unexpected{
            make_error(error_code::not_initialized,
                       "rfmod install: content store is not open")};
    }
    // One writer per content store: a concurrent startup bootstrap / GC would
    // otherwise interleave `blobs.idx`/RFRX publications. The startup path
    // already holds the lock and passes `lock_store = false`.
    std::optional<data::store_lock> lock;
    if (options.lock_store) {
        auto acquired{data::store_lock::acquire(
            store.root(), 5000, [](std::string_view line) {
                fmt::println(stderr, "mods install: {}", line);
            })};
        if (!acquired) {
            return std::unexpected{
                annotate(std::move(acquired.error()), "rfmod install")};
        }
        lock.emplace(std::move(*acquired));
    }
    // Full package verification (structure, [files] coverage, signature policy)
    // happens before the first CAS write: a rejected package publishes no blob.
    auto verified{verify_rfmod(
        reader,
        rfmod_verify_options{.trusted_keys = options.trusted_keys,
                             .require_signature = options.require_signature})};
    if (!verified) {
        return std::unexpected{
            annotate(std::move(verified.error()), "rfmod install")};
    }

    std::map<std::string, const rfmod_file_record *, std::less<>> records;
    for (const auto &record : verified->files) {
        records.emplace(record.path, &record);
    }

    rfmod_install_result result;
    result.origin = options.origin;
    result.manifest = std::move(verified->manifest);
    result.archive_digest = verified->archive_digest;
    result.manifest_digest = verified->manifest_digest;
    result.signed_package = verified->signed_package;
    result.trusted = verified->trusted;
    result.key_id = std::move(verified->key_id);
    result.public_key_fingerprint = std::move(verified->public_key_fingerprint);
    result.receipt_policy =
        receipt_policy_text(result.signed_package, options.signature_policy);
    result.files.reserve(reader.file_count());

    // Build/validate the receipt path before the first CAS write so an
    // unsafe id/version fails without leaving any blob behind (same guarantee
    // as a verification failure). The receipt itself is still published only
    // after the batch commit and the merged blob table.
    std::optional<std::string> target_receipt;
    if (options.write_receipt) {
        auto path{receipt_path_for(store, result.manifest)};
        if (!path) {
            return std::unexpected{
                annotate(std::move(path.error()), "rfmod install")};
        }
        target_receipt = std::move(*path);
    }

    // Remove staging debris of an interrupted install before doing anything
    // else. The store lock (held above or by the caller) guarantees that no
    // live installer is between creating its staging directory and writing the
    // owner record, so anything stale belongs to a crashed process.
    if (options.prune_staging) {
        auto pruned{
            prune_staging(staging_root(store.root()), result.pruned_staging)};
        if (!pruned) {
            return std::unexpected{
                annotate(std::move(pruned.error()), "rfmod install")};
        }
    }

    // Android-style update policy. All checks are read-only: a rejected update
    // leaves the installed state untouched.
    update_decision decision;
    if (options.write_receipt) {
        const auto &common{result.manifest.common};
        auto state{load_installed_state(store.root(), common.id)};
        if (!state) {
            return std::unexpected{
                annotate(std::move(state.error()), "rfmod install")};
        }
        auto decided{decide_update(
            store.root(), common.id, common.version, result.archive_digest,
            package_signer{.signed_package = result.signed_package,
                           .key_id = result.key_id,
                           .fingerprint = result.public_key_fingerprint},
            *state, options)};
        if (!decided) {
            return std::unexpected{
                annotate(std::move(decided.error()), "rfmod install")};
        }
        decision = std::move(*decided);
        result.downgrade_override = decision.downgrade_override;
        result.same_version_override = decision.same_version_override;
        result.signer_rotated = decision.signer_rotated;
        if (decision.already_installed) {
            // Identical archive already installed: no blob, table, receipt or
            // pin write. The staging directory was never created. The result
            // still reports the installed members (from the receipt) so a
            // caller sees the same shape as a fresh install.
            for (const auto &file : decision.existing_files) {
                result.files.push_back(installed_file{
                    .path = file.path,
                    .digest = file.blake3,
                    .size = file.size,
                    .mode = file.mode,
                });
            }
            result.stored_blobs = result.files.size();
            result.already_installed = true;
            result.receipt_path = std::move(decision.existing_receipt);
            return result;
        }
        if (decision.next_pin.has_value()) {
            result.receipt_policy =
                std::string{to_string(decision.next_pin->policy)};
        }
    }

    // Snapshot the previous blob table before the first write: it is both the
    // dedupe source for the batch and the metadata carried forward when the
    // merged table is republished after commit. A damaged/foreign table fails
    // here, before any staging directory, segment or blob is published.
    auto existing_table{load_install_blob_table(store)};
    if (!existing_table) {
        return std::unexpected{
            annotate(std::move(existing_table.error()), "rfmod install")};
    }
    // Existing resolution indexes are bound to the current table's BLAKE3
    // prefix. Load them before the first write (a damaged index or an index
    // without its table fails here) and re-encode them against the merged
    // table after commit, so publishing a larger blobs.idx does not invalidate
    // every RFRX.
    auto existing_indexes{
        load_install_resolution_indexes(store, *existing_table)};
    if (!existing_indexes) {
        return std::unexpected{
            annotate(std::move(existing_indexes.error()), "rfmod install")};
    }

    // Staging unpack: every member is written below one staging directory and
    // re-hashed there before it may enter the CAS. The guard removes the tree
    // on every return path, including errors.
    auto staging_directory{create_staging_directory(
        store, result.manifest.common.id, result.manifest.common.version)};
    if (!staging_directory) {
        return std::unexpected{
            annotate(std::move(staging_directory.error()), "rfmod install")};
    }
    staging_guard staging{std::move(*staging_directory)};
    if (auto owner{write_staging_owner(staging.path())}; !owner) {
        return std::unexpected{
            annotate(std::move(owner.error()), "rfmod install: staging")};
    }
    const auto members{fmt::format("{}/members", staging.path())};
    if (auto created{io::create_directories(members)}; !created) {
        return std::unexpected{
            annotate(std::move(created.error()), "rfmod install: staging")};
    }

    std::vector<std::pair<std::string, std::string>> staged_members;
    staged_members.reserve(reader.file_count());
    std::size_t staged_index{0};
    for (const auto &entry : reader.entries()) {
        auto bytes{reader.read(entry.path)};
        if (!bytes) {
            return std::unexpected{std::move(bytes.error())};
        }
        // The CAS key is authoritative: it must equal the [files] record (or,
        // for manifest.toml, the verified manifest digest).
        const auto actual{data::blake3(*bytes)};
        data::digest32 expected{actual};
        std::uint32_t mode{0644};
        if (entry.path != rfmod_manifest_path) {
            const auto found{records.find(entry.path)};
            if (found == records.end()) {
                return std::unexpected{make_error(
                    error_code::parse_error,
                    fmt::format("rfmod install: member '{}' has no [files] "
                                "record",
                                entry.path))};
            }
            expected = found->second->blake3;
            mode = found->second->mode;
        } else {
            expected = verified->manifest_digest;
        }
        if (actual != expected) {
            return std::unexpected{make_error(
                error_code::parse_error,
                fmt::format("rfmod install: BLAKE3 of '{}' does not match the "
                            "package manifest",
                            entry.path))};
        }
        // Flat staging names: package member paths are tar paths and may be
        // longer than a filesystem name allows (PAX/GNU long names), so the
        // staging file is numbered instead of mirroring the member path.
        const auto staged_path{
            fmt::format("{}/{:08}", members, staged_index++)};
        if (auto written{io::atomic_write_file(staged_path, *bytes)};
            !written) {
            return std::unexpected{
                annotate(std::move(written.error()),
                         fmt::format("rfmod install: staging {}", entry.path))};
        }
        result.files.push_back(installed_file{.path = entry.path,
                                              .digest = expected,
                                              .size = entry.size,
                                              .mode = mode});
        staged_members.emplace_back(entry.path, staged_path);
    }

    // One install transaction = one batch: small raw members share immutable
    // segments, packed members keep one file each. Bytes are ingested from the
    // staging tree, so the CAS never receives anything the staging pass did not
    // hash.
    auto staged{data::batch::begin(store.root(),
                                   existing_table && existing_table->has_value()
                                       ? &**existing_table
                                       : nullptr,
                                   install_segment_target_bytes)};
    if (!staged) {
        return std::unexpected{
            annotate(std::move(staged.error()), "rfmod install")};
    }
    for (const auto &[path, staged_path] : staged_members) {
        auto digest{staged->put_file(staged_path)};
        if (!digest) {
            return std::unexpected{
                annotate(std::move(digest.error()),
                         fmt::format("rfmod install: {}", path))};
        }
        const auto found{std::find_if(
            result.files.begin(), result.files.end(),
            [&path](const installed_file &file) { return file.path == path; })};
        if (found == result.files.end() || found->digest != *digest) {
            return std::unexpected{make_error(
                error_code::parse_error,
                fmt::format("rfmod install: CAS digest for '{}' differs from "
                            "the package manifest",
                            path))};
        }
        ++result.stored_blobs;
    }

    // Seal the last segment. Only a successful commit makes the new blob
    // records final; until then no table/receipt points at them.
    if (auto committed{staged->commit()}; !committed) {
        return std::unexpected{
            annotate(std::move(committed.error()), "rfmod install")};
    }
    const auto created{staged->new_blobs()};
    result.new_blobs = created.size();

    // Commit point: publish the merged blob table (and every re-encoded
    // resolution index) before the receipt so a reinstall can dedupe against
    // it. Existing records are preserved; new records carry this batch's
    // segment locations. The table is digest-sorted, so new records can shift
    // blob_index positions, and the new table file changes the RFRX prefix
    // binding; all index bytes are therefore encoded in memory first, then the
    // table is published, then the indexes (the same order as `content build`).
    // A failure is `io_error` and leaves the already-sealed blobs/segments in
    // place (append-only); a failure between the table and index writes leaves
    // a fail-closed pair, exactly like `content build`.
    if (!created.empty()) {
        auto table_bytes{build_install_blob_table(*existing_table, created)};
        if (!table_bytes) {
            return std::unexpected{
                annotate(std::move(table_bytes.error()), "rfmod install")};
        }
        auto next_table{data::parse_blob_table(*table_bytes)};
        if (!next_table) {
            return std::unexpected{
                annotate(std::move(next_table.error()), "rfmod install")};
        }
        std::vector<rebuilt_resolution_index> rebuilt_indexes;
        if (!existing_indexes->empty()) {
            auto rebuilt{rebuild_install_resolution_indexes(
                *existing_indexes, **existing_table, *next_table)};
            if (!rebuilt) {
                return std::unexpected{
                    annotate(std::move(rebuilt.error()), "rfmod install")};
            }
            rebuilt_indexes = std::move(*rebuilt);
        }
        if (auto written{
                io::atomic_write_file(blob_table_path(store), *table_bytes)};
            !written) {
            return std::unexpected{annotate(std::move(written.error()),
                                            "rfmod install: blobs.idx")};
        }
        for (const auto &index : rebuilt_indexes) {
            if (auto written{io::atomic_write_file(index.path, index.bytes)};
                !written) {
                return std::unexpected{
                    annotate(std::move(written.error()),
                             "rfmod install: " + index.path)};
            }
        }
    }

    // Receipt: published only after the batch committed and
    // the blob table is visible. A receipt failure is reported as `io_error`
    // but leaves those blobs in place: the store is append-only and install
    // has no rollback (GC/reference counting stays deferred). The previous
    // install stays intact because its receipt was never touched.
    if (target_receipt.has_value()) {
        const auto directory{
            fmt::format("{}/mods/{}", store.root(), result.manifest.common.id)};
        if (auto created_directory{io::create_directories(directory)};
            !created_directory) {
            return std::unexpected{
                annotate(std::move(created_directory.error()),
                         "rfmod install: receipt")};
        }
        const auto installed_at{options.installed_at.empty()
                                    ? utc_now_text()
                                    : options.installed_at};
        const auto receipt{format_install_receipt(result, installed_at,
                                                  options.installer_version)};
        if (auto written{io::atomic_write_text(*target_receipt, receipt)};
            !written) {
            return std::unexpected{
                annotate(std::move(written.error()), "rfmod install: receipt")};
        }
        result.receipt_path = std::move(*target_receipt);
    }

    // Signer pin: written after the receipt so the pin never describes an
    // install that does not exist. A pin write failure is reported but the
    // install itself is committed; the next install derives the same pin from
    // the receipt.
    if (options.write_receipt && options.write_pin &&
        decision.next_pin.has_value()) {
        const auto &signer{*decision.next_pin};
        auto path{signer_pin_path(store.root(), signer.id)};
        if (!path) {
            return std::unexpected{
                annotate(std::move(path.error()), "rfmod install: signer pin")};
        }
        const auto directory{fmt::format("{}/mods/pins", store.root())};
        if (auto created_directory{io::create_directories(directory)};
            !created_directory) {
            return std::unexpected{
                annotate(std::move(created_directory.error()),
                         "rfmod install: signer pin")};
        }
        if (auto written{
                io::atomic_write_text(*path, format_signer_pin(signer))};
            !written) {
            return std::unexpected{annotate(
                std::move(written.error()),
                "rfmod install: the install is committed but the signer pin "
                "could not be written")};
        }
        result.signer_pinned = true;
        result.signer_pin_path = std::move(*path);
    }
    return result;
}

namespace {

[[nodiscard]] auto receipt_error(std::string_view path, std::string message)
    -> error {
    return make_error(error_code::parse_error,
                      fmt::format("mod receipt: '{}': {}", path, message));
}

[[nodiscard]] auto receipt_field(const data::json::value &receipt,
                                 std::string_view field, std::string_view path)
    -> result<const data::json::value *> {
    const auto *member{receipt.find(field)};
    if (member == nullptr) {
        return std::unexpected{
            receipt_error(path, fmt::format("missing field '{}'", field))};
    }
    return member;
}

[[nodiscard]] auto receipt_string(const data::json::value &receipt,
                                  std::string_view field, std::string_view path)
    -> result<std::string> {
    auto member{receipt_field(receipt, field, path)};
    if (!member) {
        return std::unexpected{std::move(member.error())};
    }
    auto text{(*member)->as_string()};
    if (!text) {
        return std::unexpected{receipt_error(
            path, fmt::format("field '{}' must be a string", field))};
    }
    return std::string{*text};
}

[[nodiscard]] auto receipt_bool(const data::json::value &receipt,
                                std::string_view field, std::string_view path)
    -> result<bool> {
    auto member{receipt_field(receipt, field, path)};
    if (!member) {
        return std::unexpected{std::move(member.error())};
    }
    auto flag{(*member)->as_bool()};
    if (!flag) {
        return std::unexpected{receipt_error(
            path, fmt::format("field '{}' must be a boolean", field))};
    }
    return *flag;
}

[[nodiscard]] auto receipt_integer(const data::json::value &receipt,
                                   std::string_view field,
                                   std::string_view path)
    -> result<std::int64_t> {
    auto member{receipt_field(receipt, field, path)};
    if (!member) {
        return std::unexpected{std::move(member.error())};
    }
    if (!(*member)->is_integer()) {
        return std::unexpected{receipt_error(
            path, fmt::format("field '{}' must be an integer", field))};
    }
    auto value{(*member)->as_int64()};
    if (!value) {
        return std::unexpected{std::move(value.error())};
    }
    return *value;
}

[[nodiscard]] auto receipt_digest(const data::json::value &receipt,
                                  std::string_view field, std::string_view path)
    -> result<data::digest32> {
    auto text{receipt_string(receipt, field, path)};
    if (!text) {
        return std::unexpected{std::move(text.error())};
    }
    auto digest{data::parse_digest(*text)};
    if (!digest) {
        return std::unexpected{receipt_error(
            path, fmt::format("field '{}' must be 64 hex characters", field))};
    }
    return *digest;
}

/// Validate an already-opened JSON document as an install receipt.
[[nodiscard]] auto parse_receipt_document(const data::json::value &document,
                                          std::string_view path)
    -> result<mod_receipt> {
    if (!document.is_object()) {
        return std::unexpected{
            receipt_error(path, "receipt must be a JSON object")};
    }
    auto schema{receipt_string(document, "schema", path)};
    if (!schema) {
        return std::unexpected{std::move(schema.error())};
    }
    if (*schema != mod_receipt_schema) {
        return std::unexpected{receipt_error(
            path, fmt::format("unsupported receipt schema '{}' (expected "
                              "'{}')",
                              *schema, mod_receipt_schema))};
    }
    auto id{receipt_string(document, "id", path)};
    if (!id) {
        return std::unexpected{std::move(id.error())};
    }
    if (id->empty()) {
        return std::unexpected{
            receipt_error(path, "field 'id' must not be empty")};
    }
    mod_receipt out;
    out.id = std::move(*id);

    auto version{receipt_string(document, "version", path)};
    if (!version) {
        return std::unexpected{std::move(version.error())};
    }
    auto parsed_version{core::semver::parse(*version)};
    if (!parsed_version) {
        return std::unexpected{receipt_error(
            path, fmt::format("invalid version '{}': {}", *version,
                              parsed_version.error().message))};
    }
    out.version = std::move(*version);

    auto abi{receipt_integer(document, "abi", path)};
    if (!abi) {
        return std::unexpected{std::move(abi.error())};
    }
    if (*abi < 0 || *abi > static_cast<std::int64_t>(
                               std::numeric_limits<std::uint32_t>::max())) {
        return std::unexpected{
            receipt_error(path, "field 'abi' is outside the uint32 range")};
    }
    out.abi = static_cast<std::uint32_t>(*abi);

    auto archive_blake3{receipt_digest(document, "archive_blake3", path)};
    if (!archive_blake3) {
        return std::unexpected{std::move(archive_blake3.error())};
    }
    out.archive_blake3 = *archive_blake3;

    auto manifest_blake3{receipt_digest(document, "manifest_blake3", path)};
    if (!manifest_blake3) {
        return std::unexpected{std::move(manifest_blake3.error())};
    }
    out.manifest_blake3 = *manifest_blake3;

    auto signed_package{receipt_bool(document, "signed", path)};
    if (!signed_package) {
        return std::unexpected{std::move(signed_package.error())};
    }
    out.signed_package = *signed_package;

    auto trusted{receipt_bool(document, "trusted", path)};
    if (!trusted) {
        return std::unexpected{std::move(trusted.error())};
    }
    out.trusted = *trusted;

    auto key_id{receipt_string(document, "key_id", path)};
    if (!key_id) {
        return std::unexpected{std::move(key_id.error())};
    }
    out.key_id = std::move(*key_id);

    auto fingerprint{receipt_string(document, "public_key_fingerprint", path)};
    if (!fingerprint) {
        return std::unexpected{std::move(fingerprint.error())};
    }
    if (!fingerprint->empty() && !is_lower_hex(*fingerprint, 64)) {
        return std::unexpected{receipt_error(
            path, "field 'public_key_fingerprint' must be empty or 64 "
                  "lowercase hex characters")};
    }
    out.public_key_fingerprint = std::move(*fingerprint);

    auto policy{receipt_string(document, "policy", path)};
    if (!policy) {
        return std::unexpected{std::move(policy.error())};
    }
    if (*policy != "strict" && *policy != "trusted-any" &&
        *policy != "unsigned") {
        return std::unexpected{receipt_error(
            path, fmt::format("unknown policy '{}' (expected strict, "
                              "trusted-any or unsigned)",
                              *policy))};
    }
    out.policy = std::move(*policy);

    auto installed_at{receipt_string(document, "installed_at", path)};
    if (!installed_at) {
        return std::unexpected{std::move(installed_at.error())};
    }
    out.installed_at = std::move(*installed_at);

    auto installer_version{receipt_string(document, "installer_version", path)};
    if (!installer_version) {
        return std::unexpected{std::move(installer_version.error())};
    }
    out.installer_version = std::move(*installer_version);

    auto origin{receipt_string(document, "origin", path)};
    if (!origin) {
        return std::unexpected{std::move(origin.error())};
    }
    auto parsed_origin{parse_mod_origin(*origin)};
    if (!parsed_origin) {
        return std::unexpected{receipt_error(
            path, fmt::format("unknown origin '{}' (expected builtin, "
                              "first_party or user)",
                              *origin))};
    }
    out.origin = *parsed_origin;

    auto files{receipt_field(document, "files", path)};
    if (!files) {
        return std::unexpected{std::move(files.error())};
    }
    auto list{(*files)->as_array()};
    if (!list) {
        return std::unexpected{
            receipt_error(path, "field 'files' must be an array")};
    }
    out.files.reserve(list->size());
    for (std::size_t index{0}; index < list->size(); ++index) {
        const auto &entry{(*list)[index]};
        if (!entry.is_object()) {
            return std::unexpected{receipt_error(
                path, fmt::format("files[{}] must be an object", index))};
        }
        auto file_path{receipt_string(entry, "path", path)};
        if (!file_path) {
            return std::unexpected{std::move(file_path.error())};
        }
        if (!is_package_path(*file_path)) {
            return std::unexpected{receipt_error(
                path, fmt::format("files[{}].path is not a safe relative "
                                  "UTF-8 path",
                                  index))};
        }
        auto size{receipt_integer(entry, "size", path)};
        if (!size) {
            return std::unexpected{std::move(size.error())};
        }
        if (*size < 0) {
            return std::unexpected{receipt_error(
                path,
                fmt::format("files[{}].size must not be negative", index))};
        }
        auto blake3{receipt_digest(entry, "blake3", path)};
        if (!blake3) {
            return std::unexpected{std::move(blake3.error())};
        }
        auto mode{receipt_integer(entry, "mode", path)};
        if (!mode) {
            return std::unexpected{std::move(mode.error())};
        }
        if (*mode < 0 || *mode > 0777) {
            return std::unexpected{receipt_error(
                path, fmt::format("files[{}].mode must be 0..0777", index))};
        }
        out.files.push_back(receipt_file{
            .path = std::move(*file_path),
            .size = static_cast<std::uint64_t>(*size),
            .blake3 = *blake3,
            .mode = static_cast<std::uint32_t>(*mode),
        });
    }
    return out;
}

} // namespace

auto read_receipt(std::string_view path) -> result<mod_receipt> {
    auto document{data::json::value::parse_file(path)};
    if (!document) {
        // The JSON/io error already names the file; keep the receipt context
        // without repeating the path.
        return std::unexpected{
            annotate(std::move(document.error()), "mod receipt")};
    }
    return parse_receipt_document(*document, path);
}

auto list_receipt_paths(std::string_view store_root)
    -> result<std::vector<std::string>> {
    if (store_root.empty()) {
        return std::unexpected{make_error(error_code::invalid_argument,
                                          "mod receipts: empty store root")};
    }
    std::vector<std::string> paths;
    const auto mods_root{data::join_path(store_root, "mods")};
    if (!io::is_directory(mods_root)) {
        return paths;
    }
    auto ids{io::list_directory(mods_root)};
    if (!ids) {
        return std::unexpected{
            annotate(std::move(ids.error()), "mod receipts")};
    }
    for (const auto &id : *ids) {
        // `pins` is an install-state directory, not a mod id; hidden
        // directories (`.staging`, `.trash`) are installer debris.
        if (id.empty() || id.front() == '.' || id == "pins") {
            continue;
        }
        const auto directory{data::join_path(mods_root, id)};
        if (!io::is_directory(directory)) {
            continue;
        }
        auto names{io::list_directory(directory)};
        if (!names) {
            return std::unexpected{annotate(std::move(names.error()),
                                            "mod receipts: " + directory)};
        }
        for (const auto &name : *names) {
            if (!name.ends_with(".json")) {
                continue;
            }
            const auto path{data::join_path(directory, name)};
            if (!io::is_regular_file(path)) {
                continue;
            }
            paths.push_back(path);
        }
    }
    std::sort(paths.begin(), paths.end());
    return paths;
}

auto list_receipt_paths_for(std::string_view store_root, std::string_view id)
    -> result<std::vector<std::string>> {
    if (!is_safe_receipt_segment(id)) {
        return std::unexpected{make_error(
            error_code::invalid_argument,
            fmt::format("mod receipts: '{}' is not a safe single path segment",
                        id))};
    }
    auto all{list_receipt_paths(store_root)};
    if (!all) {
        return std::unexpected{std::move(all.error())};
    }
    const auto prefix{fmt::format("{}/mods/{}/", store_root, id)};
    std::vector<std::string> paths;
    for (auto &path : *all) {
        if (path.starts_with(prefix)) {
            paths.push_back(std::move(path));
        }
    }
    return paths;
}

auto receipt_path(std::string_view store_root, std::string_view id,
                  std::string_view version) -> result<std::string> {
    if (!is_safe_receipt_segment(id) || !is_safe_receipt_segment(version)) {
        return std::unexpected{make_error(
            error_code::invalid_argument,
            fmt::format("mod receipt path: id '{}' or version '{}' is not a "
                        "safe single path segment",
                        id, version))};
    }
    if (store_root.empty()) {
        return std::unexpected{
            make_error(error_code::invalid_argument,
                       "mod receipt path: empty store root")};
    }
    return fmt::format("{}/mods/{}/{}.json", store_root, id, version);
}

auto public_key_fingerprint(const data::ed25519_public_key &key)
    -> std::string {
    return data::to_hex(data::blake3(std::span<const std::byte>{key.bytes}));
}

auto to_string(rfmod_signature_policy policy) noexcept -> std::string_view {
    switch (policy) {
    case rfmod_signature_policy::strict:
        return "strict";
    case rfmod_signature_policy::trusted_any:
        return "trusted-any";
    }
    return "strict";
}

auto parse_rfmod_signature_policy(std::string_view text) noexcept
    -> std::optional<rfmod_signature_policy> {
    if (text == "strict") {
        return rfmod_signature_policy::strict;
    }
    if (text == "trusted-any") {
        return rfmod_signature_policy::trusted_any;
    }
    return std::nullopt;
}

auto read_signer_pin(std::string_view store_root, std::string_view id)
    -> result<std::optional<mod_signer_pin>> {
    auto path{signer_pin_path(store_root, id)};
    if (!path) {
        return std::unexpected{std::move(path.error())};
    }
    if (!io::exists(*path)) {
        return std::optional<mod_signer_pin>{};
    }
    auto document{data::json::value::parse_file(*path)};
    if (!document) {
        return std::unexpected{
            annotate(std::move(document.error()), "mod signer pin")};
    }
    if (!document->is_object()) {
        return std::unexpected{
            receipt_error(*path, "pin must be a JSON object")};
    }
    auto field_string = [&](std::string_view field) {
        return receipt_string(*document, field, *path);
    };
    auto schema{field_string("schema")};
    if (!schema) {
        return std::unexpected{std::move(schema.error())};
    }
    if (*schema != mod_signer_pin_schema) {
        return std::unexpected{receipt_error(
            *path, fmt::format("unsupported pin schema '{}'", *schema))};
    }
    mod_signer_pin pin;
    auto pin_id{field_string("id")};
    if (!pin_id) {
        return std::unexpected{std::move(pin_id.error())};
    }
    if (*pin_id != id) {
        return std::unexpected{receipt_error(
            *path, fmt::format("pin records id '{}' but lives under '{}'",
                               *pin_id, id))};
    }
    pin.id = std::move(*pin_id);
    auto key_id{field_string("key_id")};
    if (!key_id) {
        return std::unexpected{std::move(key_id.error())};
    }
    if (key_id->empty()) {
        return std::unexpected{
            receipt_error(*path, "field 'key_id' must not be empty")};
    }
    pin.key_id = std::move(*key_id);
    auto fingerprint{field_string("public_key_fingerprint")};
    if (!fingerprint) {
        return std::unexpected{std::move(fingerprint.error())};
    }
    if (!fingerprint->empty() && !is_lower_hex(*fingerprint, 64)) {
        return std::unexpected{receipt_error(
            *path, "field 'public_key_fingerprint' must be empty or 64 "
                   "lowercase hex characters")};
    }
    pin.public_key_fingerprint = std::move(*fingerprint);
    auto policy{field_string("policy")};
    if (!policy) {
        return std::unexpected{std::move(policy.error())};
    }
    auto parsed_policy{parse_rfmod_signature_policy(*policy)};
    if (!parsed_policy) {
        return std::unexpected{receipt_error(
            *path, fmt::format("unknown pin policy '{}'", *policy))};
    }
    pin.policy = *parsed_policy;
    auto pinned_at{field_string("pinned_at")};
    if (!pinned_at) {
        return std::unexpected{std::move(pinned_at.error())};
    }
    pin.pinned_at = std::move(*pinned_at);
    auto source{field_string("source")};
    if (!source) {
        return std::unexpected{std::move(source.error())};
    }
    pin.source = std::move(*source);
    return std::optional<mod_signer_pin>{std::move(pin)};
}

auto write_signer_pin(std::string_view store_root, const mod_signer_pin &pin)
    -> status {
    auto path{signer_pin_path(store_root, pin.id)};
    if (!path) {
        return std::unexpected{std::move(path.error())};
    }
    if (pin.key_id.empty()) {
        return std::unexpected{
            make_error(error_code::invalid_argument,
                       "mod signer pin: a pin must name a key id")};
    }
    if (!pin.public_key_fingerprint.empty() &&
        !is_lower_hex(pin.public_key_fingerprint, 64)) {
        return std::unexpected{make_error(
            error_code::invalid_argument,
            "mod signer pin: the fingerprint must be empty or 64 lowercase "
            "hex characters")};
    }
    if (auto created{
            io::create_directories(data::join_path(store_root, "mods/pins"))};
        !created) {
        return std::unexpected{
            annotate(std::move(created.error()), "mod signer pin")};
    }
    auto document{pin};
    if (document.pinned_at.empty()) {
        document.pinned_at = utc_now_text();
    }
    if (document.source.empty()) {
        document.source = "admin";
    }
    return io::atomic_write_text(*path, format_signer_pin(document));
}

auto clear_signer_pin(std::string_view store_root, std::string_view id)
    -> status {
    auto path{signer_pin_path(store_root, id)};
    if (!path) {
        return std::unexpected{std::move(path.error())};
    }
    if (!io::exists(*path)) {
        return status_ok();
    }
    std::error_code error;
    std::filesystem::remove(std::filesystem::path{*path}, error);
    if (error) {
        return std::unexpected{
            make_error(error_code::io_error,
                       fmt::format("mod signer pin: failed to remove '{}': {}",
                                   *path, error.message()))};
    }
    return status_ok();
}

auto uninstall_rfmod(data::content_store &store, std::string_view id,
                     std::optional<std::string_view> version,
                     const rfmod_uninstall_options &options)
    -> result<rfmod_uninstall_result> {
    if (!store.valid()) {
        return std::unexpected{
            make_error(error_code::not_initialized,
                       "rfmod uninstall: content store is not open")};
    }
    if (!is_safe_receipt_segment(id)) {
        return std::unexpected{make_error(
            error_code::invalid_argument,
            fmt::format("rfmod uninstall: '{}' is not a safe single path "
                        "segment",
                        id))};
    }
    if (version.has_value() && !is_safe_receipt_segment(*version)) {
        return std::unexpected{make_error(
            error_code::invalid_argument,
            fmt::format("rfmod uninstall: version '{}' is not a safe single "
                        "path segment",
                        *version))};
    }
    std::optional<data::store_lock> lock;
    if (options.lock_store) {
        auto acquired{data::store_lock::acquire(
            store.root(), 5000, [](std::string_view line) {
                fmt::println(stderr, "mods uninstall: {}", line);
            })};
        if (!acquired) {
            return std::unexpected{
                annotate(std::move(acquired.error()), "rfmod uninstall")};
        }
        lock.emplace(std::move(*acquired));
    }

    auto paths{list_receipt_paths_for(store.root(), id)};
    if (!paths) {
        return std::unexpected{
            annotate(std::move(paths.error()), "rfmod uninstall")};
    }
    if (paths->empty()) {
        return std::unexpected{make_error(
            error_code::not_found,
            fmt::format("rfmod uninstall: mod '{}' is not installed", id))};
    }

    // Select the receipts to remove. The version matches the receipt version
    // text exactly (the receipt file name); a missing version is `not_found`
    // instead of removing nothing silently.
    std::vector<std::string> targets;
    rfmod_uninstall_result result;
    result.id = std::string{id};
    if (version.has_value()) {
        const auto wanted{std::find_if(
            paths->begin(), paths->end(), [&](const std::string &path) {
                const auto name{data::filename(path)};
                return name == fmt::format("{}.json", *version);
            })};
        if (wanted == paths->end()) {
            return std::unexpected{make_error(
                error_code::not_found,
                fmt::format("rfmod uninstall: mod '{}' version '{}' is not "
                            "installed",
                            id, *version))};
        }
        targets.push_back(*wanted);
    } else {
        targets = *paths;
    }

    // Remove every selected receipt. A failure leaves the already-removed
    // receipts removed: uninstall of several versions is not one transaction
    // (each receipt is independent), and the store never records partial
    // versions.
    for (const auto &path : targets) {
        auto receipt{read_receipt(path)};
        if (!receipt) {
            return std::unexpected{
                annotate(std::move(receipt.error()), "rfmod uninstall")};
        }
        std::error_code error;
        std::filesystem::remove(std::filesystem::path{path}, error);
        if (error) {
            return std::unexpected{annotate(
                make_error(error_code::io_error,
                           fmt::format("failed to remove the receipt '{}': {}",
                                       path, error.message())),
                "rfmod uninstall")};
        }
        result.versions.push_back(std::move(receipt->version));
    }
    std::sort(result.versions.begin(), result.versions.end());
    auto remaining{list_receipt_paths_for(store.root(), id)};
    if (!remaining) {
        return std::unexpected{
            annotate(std::move(remaining.error()), "rfmod uninstall")};
    }
    result.last_version = remaining->empty();
    if (!result.last_version) {
        return result;
    }

    // Last installed version: the signer pin goes away unless the caller keeps
    // it. Keeping it requires a signer to keep; when the removed receipts
    // carry one, rewrite the pin so it survives the receipt deletion.
    auto pin_path{signer_pin_path(store.root(), id)};
    if (!pin_path) {
        return std::unexpected{std::move(pin_path.error())};
    }
    if (options.keep_signer_pin) {
        result.signer_pin_kept = true;
        result.signer_pin_path = *pin_path;
        if (!io::exists(*pin_path)) {
            // Derive from the removed receipts: the highest signed receipt
            // names the signer. A store whose receipts are unsigned has no
            // signer to keep.
            std::optional<mod_signer_pin> derived;
            std::optional<core::semver> derived_version;
            for (const auto &path : targets) {
                auto receipt{read_receipt(path)};
                if (!receipt || !receipt->pinned()) {
                    continue;
                }
                auto parsed{core::semver::parse(receipt->version)};
                if (!parsed) {
                    continue;
                }
                if (!derived.has_value() ||
                    core::compare(*parsed, *derived_version) ==
                        std::strong_ordering::greater) {
                    derived = mod_signer_pin{
                        .id = std::string{id},
                        .key_id = receipt->key_id,
                        .public_key_fingerprint =
                            receipt->public_key_fingerprint,
                        .policy = parse_rfmod_signature_policy(receipt->policy)
                                      .value_or(rfmod_signature_policy::strict),
                        .pinned_at = utc_now_text(),
                        .source = "uninstall"};
                    derived_version = std::move(*parsed);
                }
            }
            if (derived.has_value()) {
                if (auto written{write_signer_pin(store.root(), *derived)};
                    !written) {
                    return std::unexpected{
                        annotate(std::move(written.error()),
                                 "rfmod uninstall: signer pin")};
                }
            } else {
                result.signer_pin_kept = false;
                result.signer_pin_path.clear();
            }
        }
        return result;
    }
    if (auto cleared{clear_signer_pin(store.root(), id)}; !cleared) {
        return std::unexpected{
            annotate(std::move(cleared.error()), "rfmod uninstall")};
    }
    if (io::exists(*pin_path)) {
        result.signer_pin_kept = true;
        result.signer_pin_path = *pin_path;
    } else {
        result.signer_pin_cleared = true;
    }
    return result;
}

} // namespace refrain::mod
