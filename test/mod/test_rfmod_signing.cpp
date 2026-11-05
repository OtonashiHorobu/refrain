// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/core/config.hpp>
#include <refrain/core/io.hpp>
#include <refrain/data/checksum.hpp>
#include <refrain/data/content_store.hpp>
#include <refrain/data/json.hpp>
#include <refrain/data/signature.hpp>
#include <refrain/mod/rfmod.hpp>
#include <refrain/platform/process.hpp>
#include <refrain/version.hpp>

#include <boost/ut.hpp>

#include <zstd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <fmt/format.h>

namespace {

using namespace boost::ut;

namespace fs = std::filesystem;

using refrain::error_code;
using refrain::data::content_store;
using refrain::data::ed25519_keypair;
using refrain::mod::installed_file;
using refrain::mod::packed_rfmod;
using refrain::mod::rfmod_input;
using refrain::mod::rfmod_install_options;
using refrain::mod::rfmod_reader;
using refrain::mod::rfmod_signing_options;
using refrain::mod::rfmod_trusted_key;
using refrain::mod::rfmod_verification;
using refrain::mod::rfmod_verify_options;
using refrain::mod::rfmod_write_options;

// RFC 8032 test-style deterministic seed: bytes 00 01 02 ... 1f.
constexpr std::string_view fixed_seed_hex{
    "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f"};
constexpr std::string_view test_key_id{"refrain-test-key"};

// ---------------------------------------------------------------------------
// scratch directories / file helpers
// ---------------------------------------------------------------------------

[[nodiscard]] auto make_workspace_path(std::string_view name) -> fs::path {
    static int counter{0};
    const auto stamp{
        std::chrono::steady_clock::now().time_since_epoch().count()};
    const auto path{fs::temp_directory_path() /
                    fmt::format("refrain_rfmod_signing_test_{}_{}_{}_{}", stamp,
                                counter++, refrain::platform::process_id(),
                                name)};
    std::error_code error;
    fs::create_directories(path, error);
    return path;
}

struct temp_workspace {
    fs::path root{};

    explicit temp_workspace(std::string_view name)
        : root{make_workspace_path(name)} {}
    temp_workspace(const temp_workspace &) = delete;
    auto operator=(const temp_workspace &) -> temp_workspace & = delete;
    ~temp_workspace() {
        std::error_code error;
        fs::remove_all(root, error);
    }

    [[nodiscard]] auto path(std::string_view relative) const -> fs::path {
        return root / fs::path{std::string{relative}};
    }

    [[nodiscard]] auto write(std::string_view relative,
                             std::string_view content) const -> bool {
        const auto target{path(relative)};
        std::error_code error;
        fs::create_directories(target.parent_path(), error);
        if (error) {
            return false;
        }
        return refrain::core::io::write_text(target.string(), content)
            .has_value();
    }
};

[[nodiscard]] auto bytes_of(std::string_view text) -> std::vector<std::byte> {
    return std::vector<std::byte>{
        reinterpret_cast<const std::byte *>(text.data()), // NOLINT
        reinterpret_cast<const std::byte *>(text.data()) + text.size()};
}

[[nodiscard]] auto text_of(std::span<const std::byte> bytes) -> std::string {
    return std::string{reinterpret_cast<const char *>(bytes.data()), // NOLINT
                       bytes.size()};
}

// --- zstd round-trip (byte-flip tampering on a real package) ---------------

[[nodiscard]] auto zstd_compress(std::span<const std::byte> data)
    -> std::vector<std::byte> {
    const auto bound{ZSTD_compressBound(data.size())};
    std::vector<std::byte> compressed(bound);
    const auto written{ZSTD_compress(compressed.data(), compressed.size(),
                                     data.data(), data.size(), 3)};
    expect(ZSTD_isError(written) == 0) << "test zstd compress failed";
    compressed.resize(ZSTD_isError(written) == 0 ? written : 0);
    return compressed;
}

[[nodiscard]] auto zstd_decompress(std::span<const std::byte> data)
    -> std::vector<std::byte> {
    const auto size{ZSTD_getFrameContentSize(data.data(), data.size())};
    expect(size != ZSTD_CONTENTSIZE_ERROR) << "test: bad frame size";
    expect(size != ZSTD_CONTENTSIZE_UNKNOWN) << "test: unknown frame size";
    if (size == ZSTD_CONTENTSIZE_ERROR || size == ZSTD_CONTENTSIZE_UNKNOWN) {
        return {};
    }
    std::vector<std::byte> tar(static_cast<std::size_t>(size));
    const auto written{
        ZSTD_decompress(tar.data(), tar.size(), data.data(), data.size())};
    expect(ZSTD_isError(written) == 0) << "test zstd decompress failed";
    tar.resize(ZSTD_isError(written) == 0 ? written : 0);
    return tar;
}

[[nodiscard]] auto as_bytes(std::string_view text) noexcept
    -> std::span<const std::byte> {
    return {reinterpret_cast<const std::byte *>(text.data()), // NOLINT
            text.size()};
}

[[nodiscard]] auto digest_hex(const refrain::data::digest32 &digest)
    -> std::string {
    return refrain::data::to_hex(digest);
}

[[nodiscard]] auto same_bytes(std::span<const std::byte> lhs,
                              std::span<const std::byte> rhs) -> bool {
    return lhs.size() == rhs.size() &&
           std::equal(lhs.begin(), lhs.end(), rhs.begin());
}

/// Count regular files below `root` (CAS blob-count assertions).
[[nodiscard]] auto count_files(const fs::path &root) -> std::size_t {
    std::error_code error;
    if (!fs::exists(root, error)) {
        return 0;
    }
    std::size_t count{0};
    for (auto iterator{fs::recursive_directory_iterator{
             root, fs::directory_options::skip_permission_denied, error}};
         iterator != fs::recursive_directory_iterator{};
         iterator.increment(error)) {
        if (error) {
            break;
        }
        std::error_code entry_error;
        if (iterator->is_regular_file(entry_error)) {
            ++count;
        }
    }
    return count;
}

/// Number of sealed `content/segments/*.rfseg` files.
[[nodiscard]] auto count_segments(const fs::path &store_root) -> std::size_t {
    std::error_code error;
    std::size_t count{0};
    fs::directory_iterator iterator{store_root / "content" / "segments", error};
    const fs::directory_iterator end{};
    while (!error && iterator != end) {
        std::error_code entry_error;
        if (iterator->path().extension() == ".rfseg" &&
            iterator->is_regular_file(entry_error) && !entry_error) {
            ++count;
        }
        iterator.increment(error);
    }
    return count;
}

/// Parse the published `<store>/content/blobs.idx` (installed members).
[[nodiscard]] auto load_installed_blob_table(const content_store &store)
    -> std::optional<refrain::data::blob_table_view> {
    const auto path{fs::path{store.root()} / "content" /
                    std::string{refrain::data::content_blob_table_filename}};
    auto bytes{refrain::core::io::read_file(path.string())};
    if (!bytes) {
        return std::nullopt;
    }
    auto parsed{refrain::data::parse_blob_table(*bytes)};
    if (!parsed) {
        return std::nullopt;
    }
    return std::optional<refrain::data::blob_table_view>{std::move(*parsed)};
}

[[nodiscard]] auto find_table_entry(const refrain::data::blob_table_view &table,
                                    const refrain::data::digest32 &digest)
    -> const refrain::data::blob_table_entry * {
    const auto found{
        std::find_if(table.entries.begin(), table.entries.end(),
                     [&digest](const refrain::data::blob_table_entry &entry) {
                         return entry.digest == digest;
                     })};
    return found == table.entries.end() ? nullptr : &*found;
}

[[nodiscard]] auto
read_installed_blob(const content_store &store,
                    const refrain::data::blob_table_view &table,
                    const refrain::data::digest32 &digest)
    -> refrain::result<std::vector<std::byte>> {
    const auto *entry{find_table_entry(table, digest)};
    if (entry == nullptr) {
        return std::unexpected{refrain::make_error(
            error_code::not_found, "installed blob is missing from blobs.idx")};
    }
    return store.read(refrain::data::blob_location_from_table_entry(*entry));
}

// ---------------------------------------------------------------------------
// keys / manifest trees
// ---------------------------------------------------------------------------

[[nodiscard]] auto keypair_from(std::string_view seed_hex) -> ed25519_keypair {
    auto seed{refrain::data::parse_seed_hex(seed_hex)};
    expect(static_cast<bool>(seed)) << "test seed is invalid";
    if (!seed) {
        return {};
    }
    auto pair{refrain::data::keypair_from_seed(*seed)};
    expect(static_cast<bool>(pair)) << "libsodium key derivation failed";
    if (!pair) {
        return {};
    }
    return *pair;
}

/// The signing key used by the signed-package tests.
[[nodiscard]] auto fixed_keypair() -> ed25519_keypair {
    return keypair_from(fixed_seed_hex);
}

/// A different valid keypair (wrong-key tests).
[[nodiscard]] auto other_keypair() -> ed25519_keypair {
    return keypair_from(std::string(64, 'a'));
}

[[nodiscard]] auto signing_for(std::string_view key_id,
                               const ed25519_keypair &pair)
    -> rfmod_signing_options {
    return rfmod_signing_options{.secret_key = pair.secret_key,
                                 .key_id = std::string{key_id}};
}

[[nodiscard]] auto trusted_for(std::string_view key_id,
                               const ed25519_keypair &pair)
    -> rfmod_trusted_key {
    return rfmod_trusted_key{.key_id = std::string{key_id},
                             .public_key = pair.public_key};
}

[[nodiscard]] auto source_manifest(std::string_view id = "refrain.test.signed",
                                   std::string_view version = "1.0.0")
    -> std::string {
    return fmt::format("[mod]\nid = \"{}\"\nversion = \"{}\"\nabi = \"1\"\n"
                       "side = \"both\"\n\n[meta]\ndisplay_name = \"RFMOD "
                       "signed test\"\n",
                       id, version);
}

struct mod_tree {
    std::string manifest{};
    std::vector<std::pair<std::string, std::string>> files{};
};

/// Content members of the fixture tree; sorted by path bytes, manifest.toml is
/// added by the packer and never appears in `[files]`.
[[nodiscard]] auto default_tree(std::string manifest) -> mod_tree {
    mod_tree tree;
    tree.manifest = std::move(manifest);
    tree.files = {
        {"LICENSE", "MIT\n"},
        {"assets/lang/en_us.json", R"({"a":"b"})"},
        {"data/refrain_test/recipe/a.json", R"({"type":"x"})"},
        {"scripts/init.wasm", "wasm-bytes"},
    };
    return tree;
}

const std::vector<std::string> expected_record_paths{
    "LICENSE", "assets/lang/en_us.json", "data/refrain_test/recipe/a.json",
    "scripts/init.wasm"};

void materialise(const temp_workspace &work, std::string_view mod_name,
                 const mod_tree &tree) {
    expect(
        work.write(fmt::format("{}/manifest.toml", mod_name), tree.manifest));
    for (const auto &[path, content] : tree.files) {
        expect(work.write(fmt::format("{}/{}", mod_name, path), content));
    }
}

// ---------------------------------------------------------------------------
// packaging helpers
// ---------------------------------------------------------------------------

struct packed_package {
    packed_rfmod info{};
    std::vector<std::byte> archive{};
    std::string manifest{}; ///< the shipped manifest.toml text
};

[[nodiscard]] auto pack(const temp_workspace &work, std::string_view mod_name,
                        std::string_view output_name,
                        const rfmod_write_options &options = {})
    -> std::optional<packed_package> {
    const auto output{work.path(output_name)};
    std::error_code error;
    fs::create_directories(output.parent_path(), error);
    auto packed{refrain::mod::pack_mod_directory(work.path(mod_name).string(),
                                                 output.string(), options)};
    expect(static_cast<bool>(packed)) << "pack_mod_directory failed";
    if (!packed) {
        return std::nullopt;
    }
    auto bytes{refrain::core::io::read_file(output.string())};
    expect(static_cast<bool>(bytes));
    if (!bytes) {
        return std::nullopt;
    }
    auto reader{rfmod_reader::open(*bytes)};
    expect(static_cast<bool>(reader));
    if (!reader) {
        return std::nullopt;
    }
    auto manifest_bytes{reader->read("manifest.toml")};
    expect(static_cast<bool>(manifest_bytes));
    if (!manifest_bytes) {
        return std::nullopt;
    }
    packed_package package;
    package.info = std::move(*packed);
    package.archive = std::move(*bytes);
    package.manifest = text_of(*manifest_bytes);
    return package;
}

/// Build a bare container around an arbitrary manifest + content set (tamper
/// and malformed-manifest tests). `build_rfmod` does not parse the manifest, so
/// the verifier is what rejects the result.
[[nodiscard]] auto
archive_from(std::string_view manifest,
             const std::vector<std::pair<std::string, std::string>> &files)
    -> std::vector<std::byte> {
    std::vector<std::pair<std::string, std::vector<std::byte>>> owned;
    owned.reserve(files.size() + 1);
    owned.emplace_back(std::string{refrain::mod::rfmod_manifest_path},
                       bytes_of(manifest));
    for (const auto &[path, content] : files) {
        owned.emplace_back(path, bytes_of(content));
    }
    std::vector<rfmod_input> inputs;
    inputs.reserve(owned.size());
    for (const auto &[path, data] : owned) {
        inputs.push_back(rfmod_input{.path = path,
                                     .data = std::span<const std::byte>{data}});
    }
    auto archive{refrain::mod::build_rfmod(inputs)};
    expect(static_cast<bool>(archive)) << "test build_rfmod failed";
    return archive ? std::move(*archive) : std::vector<std::byte>{};
}

[[nodiscard]] auto replace_once(std::string text, std::string_view from,
                                std::string_view to) -> std::string {
    const auto position{text.find(from)};
    expect(position != std::string::npos)
        << "test pattern not found: " << std::string{from};
    if (position == std::string::npos) {
        return text;
    }
    text.replace(position, from.size(), to);
    return text;
}

/// The `[files]` record line for `path` (without the trailing newline).
[[nodiscard]] auto record_line(std::string_view manifest, std::string_view path)
    -> std::string_view {
    const auto needle{fmt::format("\"{}\" = {{", path)};
    const auto position{manifest.find(needle)};
    expect(position != std::string_view::npos)
        << "test record not found: " << std::string{path};
    if (position == std::string_view::npos) {
        return {};
    }
    const auto end{manifest.find('\n', position)};
    return manifest.substr(position, end == std::string_view::npos
                                         ? std::string_view::npos
                                         : end - position);
}

/// Replace one field value inside a `[files]` record.
[[nodiscard]] auto
rewrite_record_field(std::string manifest, std::string_view path,
                     std::string_view field, std::string_view replacement)
    -> std::string {
    const auto line{record_line(manifest, path)};
    if (line.empty()) {
        return manifest;
    }
    const auto marker{line.find(fmt::format("{} = ", field))};
    expect(marker != std::string_view::npos)
        << "test field not found: " << std::string{field};
    if (marker == std::string_view::npos) {
        return manifest;
    }
    const auto value_begin{marker + field.size() + 3U};
    auto value_end{line.find_first_of(",}", value_begin)};
    if (value_end == std::string_view::npos) {
        value_end = line.size();
    }
    auto value{line.substr(value_begin, value_end - value_begin)};
    while (!value.empty() && value.back() == ' ') {
        value.remove_suffix(1);
    }
    if (value.empty()) {
        return manifest;
    }
    const auto offset{static_cast<std::size_t>(line.data() - manifest.data()) +
                      value_begin};
    manifest.replace(offset, value.size(), replacement);
    return manifest;
}

[[nodiscard]] auto remove_record(std::string manifest, std::string_view path)
    -> std::string {
    const auto line{record_line(manifest, path)};
    if (line.empty()) {
        return manifest;
    }
    const auto offset{static_cast<std::size_t>(line.data() - manifest.data())};
    const auto end{manifest.find('\n', offset)};
    manifest.erase(offset,
                   (end == std::string::npos ? manifest.size() : end + 1U) -
                       offset);
    return manifest;
}

/// Insert a record line at the end of `[files]` (before `[packaging]`).
[[nodiscard]] auto insert_record(std::string manifest, std::string_view record)
    -> std::string {
    const auto marker{manifest.find("[packaging]")};
    expect(marker != std::string::npos);
    if (marker == std::string::npos) {
        return manifest;
    }
    manifest.insert(marker, fmt::format("{}\n", record));
    return manifest;
}

[[nodiscard]] auto record_for(std::string_view path, std::string_view content)
    -> std::string {
    return fmt::format(
        "\"{}\" = {{ size = {}, blake3 = \"{}\", mode = 0o644 }}", path,
        content.size(),
        refrain::data::to_hex(refrain::data::blake3(as_bytes(content))));
}

/// Offset of the canonical `[signature]` header line in a shipped manifest.
[[nodiscard]] auto signature_offset(std::string_view manifest) -> std::size_t {
    const auto position{manifest.rfind("[signature]")};
    expect(position != std::string_view::npos);
    return position == std::string_view::npos ? 0 : position;
}

[[nodiscard]] auto open_archive(std::span<const std::byte> bytes)
    -> refrain::result<rfmod_reader> {
    return rfmod_reader::open(bytes);
}

/// Convenience: verify an archive with the fixed signing key trusted.
[[nodiscard]] auto verify_with_fixed_key(const rfmod_reader &reader,
                                         bool require_signature = true)
    -> refrain::result<rfmod_verification> {
    return refrain::mod::verify_rfmod(
        reader, rfmod_verify_options{
                    .trusted_keys = {trusted_for(test_key_id, fixed_keypair())},
                    .require_signature = require_signature});
}

} // namespace

namespace {

using namespace boost::ut;

// ---------------------------------------------------------------------------
// packing
// ---------------------------------------------------------------------------

const suite rfmod_signing_pack_tests = [] {
    "rfmod/signed pack embeds [files], [packaging] and [signature]"_test = [] {
        temp_workspace work{"signed-pack"};
        const auto keys{fixed_keypair()};
        materialise(work, "mod", default_tree(source_manifest()));
        rfmod_write_options options;
        options.signing = signing_for(test_key_id, keys);
        auto package{pack(work, "mod", "out/mod.rfmod", options)};
        expect(package.has_value());
        if (!package) {
            return;
        }
        const auto &text{package->manifest};

        auto doc{refrain::core::config::document::parse_text(text)};
        expect(static_cast<bool>(doc)) << "shipped manifest is not valid TOML";
        if (!doc) {
            return;
        }

        // [files] lists every content member, sorted, and never manifest.toml.
        const std::array<std::string_view, 1> files_path{"files"};
        auto paths{doc->keys_at(files_path)};
        expect(static_cast<bool>(paths));
        if (paths) {
            expect(*paths == expected_record_paths) << "[files] is not the "
                                                       "sorted content set";
            const auto listed_manifest{
                std::find(paths->begin(), paths->end(), "manifest.toml")};
            expect(listed_manifest == paths->end());
        }
        expect(!doc->contains_at(
            std::array<std::string_view, 2>{"files", "manifest.toml"}));

        // Records are also written in sorted order in the file itself (the
        // getter sorts defensively, so check the raw bytes too).
        std::size_t previous_position{0};
        for (const auto &path : expected_record_paths) {
            const auto position{text.find(fmt::format("\"{}\" = {{", path))};
            expect(position != std::string::npos)
                << "missing record for " << path;
            expect(position > previous_position)
                << "records are not sorted by path bytes: " << path;
            previous_position = position;
        }

        // Per-file record: size / BLAKE3 / mode.
        const std::array<std::string_view, 3> license_size{"files", "LICENSE",
                                                           "size"};
        const std::array<std::string_view, 3> license_hash{"files", "LICENSE",
                                                           "blake3"};
        const std::array<std::string_view, 3> license_mode{"files", "LICENSE",
                                                           "mode"};
        auto size{doc->get_int64_at(license_size)};
        expect(static_cast<bool>(size));
        if (size) {
            expect(*size == 4);
        }
        auto hash{doc->get_string_at(license_hash)};
        expect(static_cast<bool>(hash));
        if (hash) {
            expect(*hash == digest_hex(refrain::data::blake3(
                                as_bytes(std::string_view{"MIT\n"}))));
        }
        auto mode{doc->get_int64_at(license_mode)};
        expect(static_cast<bool>(mode));
        if (mode) {
            expect(*mode == 0644);
        }

        // [packaging] is the deterministic build record.
        auto tool{doc->get_string_at(
            std::array<std::string_view, 2>{"packaging", "tool"})};
        expect(static_cast<bool>(tool));
        if (tool) {
            expect(*tool == "refrain-tools");
        }
        auto tool_version{doc->get_string_at(
            std::array<std::string_view, 2>{"packaging", "tool_version"})};
        expect(static_cast<bool>(tool_version));
        if (tool_version) {
            // The release SemVer only: branch/commit would break byte-for-byte
            // reproducibility.
            expect(*tool_version == refrain::version::release_version);
        }
        auto format_version{doc->get_int64_at(
            std::array<std::string_view, 2>{"packaging", "format_version"})};
        expect(static_cast<bool>(format_version));
        if (format_version) {
            expect(*format_version == 1);
        }
        auto deterministic{doc->get_bool_at(
            std::array<std::string_view, 2>{"packaging", "deterministic"})};
        expect(static_cast<bool>(deterministic));
        if (deterministic) {
            expect(*deterministic);
        }

        // [signature] is unique and the last table of the file.
        expect(text.find("[signature]") == text.rfind("[signature]"));
        expect(text.rfind("[signature]") > text.rfind("[packaging]"));

        // The signature covers exactly the bytes before the header line.
        const auto offset{signature_offset(text)};
        auto signature_hex{doc->get_string_at(
            std::array<std::string_view, 2>{"signature", "sig"})};
        expect(static_cast<bool>(signature_hex));
        if (signature_hex) {
            expect(signature_hex->size() == 128U);
            auto signature{refrain::data::parse_signature_hex(*signature_hex)};
            expect(static_cast<bool>(signature));
            if (signature) {
                const auto prefix{std::string_view{text}.substr(0, offset)};
                expect(static_cast<bool>(refrain::data::verify(
                    as_bytes(prefix), *signature, keys.public_key)));
            }
        }

        // Byte-identical repack (Ed25519 is deterministic).
        auto again{pack(work, "mod", "out/mod2.rfmod", options)};
        expect(again.has_value());
        if (again) {
            expect(same_bytes(package->archive, again->archive))
                << "repacking the same tree changed the bytes";
            expect(again->manifest == text);
        }
    };

    "rfmod/unsigned pack still embeds [files] and [packaging]"_test = [] {
        temp_workspace work{"unsigned-pack"};
        materialise(work, "mod", default_tree(source_manifest()));
        auto package{pack(work, "mod", "out/mod.rfmod")};
        expect(package.has_value());
        if (!package) {
            return;
        }
        expect(package->manifest.find("[files]") != std::string::npos);
        expect(package->manifest.find("[packaging]") != std::string::npos);
        expect(package->manifest.find("[signature]") == std::string::npos);

        auto reader{open_archive(package->archive)};
        expect(static_cast<bool>(reader));
        if (!reader) {
            return;
        }
        auto verified{refrain::mod::verify_rfmod(*reader)};
        expect(static_cast<bool>(verified)) << "unsigned package must verify";
        if (!verified) {
            return;
        }
        expect(verified->signed_package == false);
        expect(verified->trusted == false);
        expect(!verified->signature.has_value());
        expect(verified->key_id.empty());
        expect(verified->files.size() == expected_record_paths.size());

        // Policy: an unsigned package is not acceptable when signatures are
        // required.
        auto required{verify_with_fixed_key(*reader, true)};
        expect(!required);
        expect(static_cast<int>(required.error().code) ==
               static_cast<int>(error_code::not_found));
    };

    "rfmod/pack handles a manifest-only mod (empty [files])"_test = [] {
        temp_workspace work{"manifest-only"};
        mod_tree tree;
        tree.manifest = source_manifest("refrain.test.empty");
        materialise(work, "mod", tree);
        auto package{pack(work, "mod", "out/mod.rfmod")};
        expect(package.has_value());
        if (!package) {
            return;
        }
        // Empty [files] table followed directly by [packaging].
        expect(package->manifest.find("[files]\n[packaging]") !=
               std::string::npos);
        auto reader{open_archive(package->archive)};
        expect(static_cast<bool>(reader));
        if (!reader) {
            return;
        }
        expect(reader->file_count() == 1U); // manifest.toml only
        auto verified{refrain::mod::verify_rfmod(*reader)};
        expect(static_cast<bool>(verified));
        if (!verified) {
            return;
        }
        expect(verified->files.empty());
        auto store{content_store::open(work.path("cas").string())};
        expect(static_cast<bool>(store));
        if (!store) {
            return;
        }
        auto installed{refrain::mod::install_rfmod(*reader, *store)};
        expect(static_cast<bool>(installed));
        if (installed) {
            expect(installed->files.size() == 1U);
            expect(installed->new_blobs == 1U);
            expect(installed->stored_blobs == 1U);
        }
    };

    "rfmod/pack rejects a source manifest with package tables"_test = [] {
        temp_workspace work{"already-packed"};
        // A manifest that already carries [files] is a packed package, not a
        // source tree: packing it again must fail.
        const auto packed_manifest{fmt::format(
            "{}\n\n[files]\n[packaging]\ntool = \"refrain-tools\"\n",
            source_manifest())};
        expect(work.write("mod/manifest.toml", packed_manifest));
        expect(work.write("mod/data/a.json", "{}"));
        const auto output{work.path("out/mod.rfmod")};
        std::error_code error;
        fs::create_directories(output.parent_path(), error);
        const auto packed{refrain::mod::pack_mod_directory(
            work.path("mod").string(), output.string())};
        expect(!packed);
        expect(static_cast<int>(packed.error().code) ==
               static_cast<int>(error_code::invalid_argument));
        expect(packed.error().message.find("package table") !=
               std::string::npos);
    };

    "rfmod/pack rejects a source manifest with a [signature] line"_test = [] {
        temp_workspace work{"already-signed"};
        expect(work.write("mod/manifest.toml",
                          fmt::format("{}\n[signature]\nscheme = "
                                      "\"ed25519\"\n",
                                      source_manifest())));
        expect(work.write("mod/data/a.json", "{}"));
        const auto output{work.path("out/mod.rfmod")};
        std::error_code error;
        fs::create_directories(output.parent_path(), error);
        const auto packed{refrain::mod::pack_mod_directory(
            work.path("mod").string(), output.string())};
        expect(!packed);
        expect(static_cast<int>(packed.error().code) ==
               static_cast<int>(error_code::invalid_argument));
    };

    "rfmod/pack rejects a path that cannot be a TOML basic-string key"_test =
        [] {
            temp_workspace work{"quoted-path"};
            expect(work.write("mod/manifest.toml", source_manifest()));
            // '"' is legal in a Linux file name, but [files] keys are written
            // unescaped: packing such a tree would succeed and then be
            // unverifiable, so it must be rejected up front.
            expect(work.write("mod/data/a\"b.json", "{}"));
            const auto output{work.path("out/mod.rfmod")};
            std::error_code error;
            fs::create_directories(output.parent_path(), error);
            const auto packed{refrain::mod::pack_mod_directory(
                work.path("mod").string(), output.string())};
            expect(!packed);
            if (!packed) {
                expect(static_cast<int>(packed.error().code) ==
                       static_cast<int>(error_code::invalid_argument));
                expect(packed.error().message.find("TOML") != std::string::npos)
                    << "message must explain the TOML escaping limit";
            }
            std::error_code exists_error;
            expect(!fs::exists(output, exists_error))
                << "a rejected tree must not produce an archive";
        };
};

// ---------------------------------------------------------------------------
// verification: positive cases
// ---------------------------------------------------------------------------

const suite rfmod_signing_verify_tests = [] {
    "rfmod/verify accepts a trusted signed package"_test = [] {
        temp_workspace work{"signed-verify"};
        const auto keys{fixed_keypair()};
        materialise(work, "mod", default_tree(source_manifest()));
        rfmod_write_options options;
        options.signing = signing_for(test_key_id, keys);
        auto package{pack(work, "mod", "out/mod.rfmod", options)};
        expect(package.has_value());
        if (!package) {
            return;
        }
        auto reader{open_archive(package->archive)};
        expect(static_cast<bool>(reader));
        if (!reader) {
            return;
        }
        auto verified{verify_with_fixed_key(*reader)};
        expect(static_cast<bool>(verified)) << "trusted signed package must "
                                               "verify";
        if (!verified) {
            return;
        }
        expect(verified->signed_package);
        expect(verified->trusted);
        expect(verified->key_id == test_key_id);
        expect(verified->signature.has_value());
        if (verified->signature) {
            expect(verified->signature->scheme == "ed25519");
            expect(verified->signature->key_id == test_key_id);
        }
        expect(verified->manifest.common.id == "refrain.test.signed");
        expect(verified->files.size() == expected_record_paths.size());
        // Records are sorted by path bytes.
        for (std::size_t index{0}; index < verified->files.size(); ++index) {
            expect(verified->files.at(index).path ==
                   expected_record_paths.at(index));
            expect(verified->files.at(index).mode == 0644U);
        }
        expect(digest_hex(verified->archive_digest) ==
               digest_hex(reader->archive_digest()));
        expect(digest_hex(verified->manifest_digest) ==
               digest_hex(refrain::data::blake3(as_bytes(package->manifest))));

        // Verification is a pure function of the package bytes.
        auto second{verify_with_fixed_key(*reader)};
        expect(static_cast<bool>(second));
        if (second) {
            expect(digest_hex(second->archive_digest) ==
                   digest_hex(verified->archive_digest));
            expect(digest_hex(second->manifest_digest) ==
                   digest_hex(verified->manifest_digest));
        }

        // Signed by an unknown key with no trust anchors: accepted for
        // third-party distribution, but never trusted.
        auto untrusted{refrain::mod::verify_rfmod(*reader)};
        expect(static_cast<bool>(untrusted));
        if (untrusted) {
            expect(untrusted->signed_package);
            expect(!untrusted->trusted);
            expect(untrusted->key_id == test_key_id);
        }

        // A signed package without a matching configured key is rejected when
        // other trust anchors exist.
        auto wrong_id{refrain::mod::verify_rfmod(
            *reader, rfmod_verify_options{
                         .trusted_keys = {trusted_for("other-key", keys)}})};
        expect(!wrong_id);
        expect(static_cast<int>(wrong_id.error().code) ==
               static_cast<int>(error_code::not_found));
        expect(wrong_id.error().message.find("untrusted key_id") !=
               std::string::npos);

        // require_signature with no trust anchors configured is a config
        // error.
        auto no_keys{refrain::mod::verify_rfmod(
            *reader, rfmod_verify_options{.require_signature = true})};
        expect(!no_keys);
        expect(static_cast<int>(no_keys.error().code) ==
               static_cast<int>(error_code::invalid_argument));
        expect(no_keys.error().message.find("no trusted keys") !=
               std::string::npos);

        // Same key_id but a different public key: signature failure.
        auto wrong_key{refrain::mod::verify_rfmod(
            *reader, rfmod_verify_options{.trusted_keys = {trusted_for(
                                              test_key_id, other_keypair())},
                                          .require_signature = true})};
        expect(!wrong_key);
        expect(static_cast<int>(wrong_key.error().code) ==
               static_cast<int>(error_code::parse_error));
        expect(wrong_key.error().message.find("verification failed") !=
               std::string::npos);
    };
};

// ---------------------------------------------------------------------------
// verification: tampering and malformed manifests
// ---------------------------------------------------------------------------

const suite rfmod_signing_rejection_tests = [] {
    "rfmod/verify rejects a tampered archive member"_test = [] {
        temp_workspace work{"tamper-member"};
        const auto keys{fixed_keypair()};
        materialise(work, "mod", default_tree(source_manifest()));
        rfmod_write_options options;
        options.signing = signing_for(test_key_id, keys);
        auto package{pack(work, "mod", "out/mod.rfmod", options)};
        expect(package.has_value());
        if (!package) {
            return;
        }

        auto tampered{default_tree(source_manifest())};
        for (auto &[path, content] : tampered.files) {
            if (path == "data/refrain_test/recipe/a.json") {
                // Same length as the signed content: only BLAKE3 can catch it.
                content = R"({"type":"y"})";
            }
        }
        auto archive{archive_from(package->manifest, tampered.files)};
        auto reader{open_archive(archive)};
        expect(static_cast<bool>(reader));
        if (!reader) {
            return;
        }
        auto verified{verify_with_fixed_key(*reader)};
        expect(!verified);
        expect(static_cast<int>(verified.error().code) ==
               static_cast<int>(error_code::parse_error));
        expect(verified.error().message.find("BLAKE3") != std::string::npos);
    };

    "rfmod/verify rejects flipped member bytes (zstd round-trip)"_test = [] {
        temp_workspace work{"tamper-flip"};
        const auto keys{fixed_keypair()};
        materialise(work, "mod", default_tree(source_manifest()));
        rfmod_write_options options;
        options.signing = signing_for(test_key_id, keys);
        auto package{pack(work, "mod", "out/mod.rfmod", options)};
        expect(package.has_value());
        if (!package) {
            return;
        }
        // Flip one byte of the payload inside the decompressed tar and
        // recompress: the member keeps its size, only BLAKE3 can catch it.
        auto tar{zstd_decompress(package->archive)};
        const auto needle{bytes_of(R"({"type":"x"})")};
        const auto found{
            std::search(tar.begin(), tar.end(), needle.begin(), needle.end())};
        expect(found != tar.end()) << "test payload not found in the tar";
        if (found == tar.end()) {
            return;
        }
        *found = std::byte{'Y'};
        const auto tampered_archive{zstd_compress(tar)};
        auto reader{open_archive(tampered_archive)};
        expect(static_cast<bool>(reader));
        if (!reader) {
            return;
        }
        auto verified{verify_with_fixed_key(*reader)};
        expect(!verified);
        expect(static_cast<int>(verified.error().code) ==
               static_cast<int>(error_code::parse_error));
        expect(verified.error().message.find("BLAKE3") != std::string::npos);
    };

    "rfmod/verify rejects a tampered signed prefix"_test = [] {
        temp_workspace work{"tamper-prefix"};
        const auto keys{fixed_keypair()};
        materialise(work, "mod", default_tree(source_manifest()));
        rfmod_write_options options;
        options.signing = signing_for(test_key_id, keys);
        auto package{pack(work, "mod", "out/mod.rfmod", options)};
        expect(package.has_value());
        if (!package) {
            return;
        }
        // Change the source section (part of the signed prefix) without
        // touching [files]; the JSON/TOML stays valid.
        const auto tampered_manifest{replace_once(
            package->manifest, "RFMOD signed test", "RFMOD signed tesu")};
        auto archive{archive_from(tampered_manifest,
                                  default_tree(source_manifest()).files)};
        auto reader{open_archive(archive)};
        expect(static_cast<bool>(reader));
        if (!reader) {
            return;
        }
        auto verified{verify_with_fixed_key(*reader)};
        expect(!verified);
        expect(static_cast<int>(verified.error().code) ==
               static_cast<int>(error_code::parse_error));
        expect(verified.error().message.find("verification failed") !=
               std::string::npos);
    };

    "rfmod/verify rejects broken [files] coverage and paths"_test = [] {
        temp_workspace work{"coverage"};
        const auto keys{fixed_keypair()};
        materialise(work, "mod", default_tree(source_manifest()));
        rfmod_write_options options;
        options.signing = signing_for(test_key_id, keys);
        auto package{pack(work, "mod", "out/mod.rfmod", options)};
        expect(package.has_value());
        if (!package) {
            return;
        }
        const auto &manifest{package->manifest};
        const auto &files{default_tree(source_manifest()).files};

        const auto expect_failure = [&files](std::string_view candidate,
                                             error_code expected,
                                             std::string_view needle) {
            auto archive{archive_from(candidate, files)};
            auto reader{open_archive(archive)};
            expect(static_cast<bool>(reader));
            if (!reader) {
                return;
            }
            auto verified{verify_with_fixed_key(*reader)};
            expect(!verified) << "expected rejection";
            if (!verified) {
                expect(static_cast<int>(verified.error().code) ==
                       static_cast<int>(expected))
                    << "error: " << verified.error().message;
                if (!needle.empty()) {
                    expect(verified.error().message.find(needle) !=
                           std::string::npos)
                        << "error: " << verified.error().message;
                }
            }
        };

        // Archive member without a record.
        expect_failure(remove_record(manifest, "LICENSE"),
                       error_code::parse_error, "missing from [files]");
        // Record without an archive member.
        expect_failure(
            insert_record(manifest, record_for("extra/file.txt", "abc")),
            error_code::parse_error, "no such member");
        // Wrong size in a record (the archive member keeps its real size).
        expect_failure(rewrite_record_field(manifest, "LICENSE", "size", "999"),
                       error_code::parse_error, "records size");
        // Wrong BLAKE3 in a record.
        expect_failure(
            rewrite_record_field(manifest, "LICENSE", "blake3",
                                 fmt::format("\"{}\"", std::string(64, '0'))),
            error_code::parse_error, "BLAKE3");
        // Unsafe paths are rejected outright.
        expect_failure(
            replace_once(manifest, "\"LICENSE\" = {", "\"../evil\" = {"),
            error_code::parse_error, "unsafe");
        expect_failure(
            replace_once(manifest, "\"LICENSE\" = {", "\"/etc/passwd\" = {"),
            error_code::parse_error, "unsafe");
        // manifest.toml cannot be listed (it cannot contain its own hash).
        expect_failure(
            insert_record(manifest, record_for("manifest.toml", "x")),
            error_code::parse_error, "must not list manifest.toml");
        // Unknown keys in a record.
        expect_failure(
            replace_once(manifest, "mode = 0o644", "mode = 0o644, extra = 1"),
            error_code::parse_error, "unknown");
        // Duplicate record: TOML duplicate keys are a parse error.
        expect_failure(insert_record(manifest, std::string{record_line(
                                                   manifest, "LICENSE")}),
                       error_code::parse_error, "");
    };

    "rfmod/verify rejects malformed package tables"_test = [] {
        temp_workspace work{"tables"};
        const auto keys{fixed_keypair()};
        materialise(work, "mod", default_tree(source_manifest()));
        rfmod_write_options options;
        options.signing = signing_for(test_key_id, keys);
        auto package{pack(work, "mod", "out/mod.rfmod", options)};
        expect(package.has_value());
        if (!package) {
            return;
        }
        const auto &manifest{package->manifest};
        const auto &files{default_tree(source_manifest()).files};

        const auto expect_failure = [&files](std::string_view candidate,
                                             error_code expected,
                                             std::string_view needle) {
            auto archive{archive_from(candidate, files)};
            auto reader{open_archive(archive)};
            expect(static_cast<bool>(reader));
            if (!reader) {
                return;
            }
            auto verified{verify_with_fixed_key(*reader)};
            expect(!verified) << "expected rejection";
            if (!verified) {
                expect(static_cast<int>(verified.error().code) ==
                       static_cast<int>(expected))
                    << "error: " << verified.error().message;
                if (!needle.empty()) {
                    expect(verified.error().message.find(needle) !=
                           std::string::npos)
                        << "error: " << verified.error().message;
                }
            }
        };

        // Container version gates.
        expect_failure(
            replace_once(manifest, "format_version = 1", "format_version = 2"),
            error_code::version_mismatch, "format_version");
        expect_failure(replace_once(manifest, "abi = \"1\"\n",
                                    "abi = \"1\"\nformat_version = 2\n"),
                       error_code::version_mismatch, "format_version");
        // [packaging] invariants.
        expect_failure(replace_once(manifest, "deterministic = true",
                                    "deterministic = false"),
                       error_code::parse_error, "deterministic");
        expect_failure(
            replace_once(manifest, "zstd_level = 3", "zstd_level = \"three\""),
            error_code::parse_error, "zstd_level");
        expect_failure(replace_once(manifest, "tool = \"refrain-tools\"",
                                    "tool = \"refrain-tools\"\nbogus = 1"),
                       error_code::parse_error, "unknown");
        // Signature block rules.
        expect_failure(fmt::format("{}\n[signature.extra]\nx = 1\n", manifest),
                       error_code::parse_error, "more than one [signature]");
        expect_failure(fmt::format("{}\n[extra]\nx = 1\n", manifest),
                       error_code::invalid_argument, "unknown top-level table");
        // An inline `signature = {...}` root key is not a trailing table.
        const std::string unsigned_manifest{
            std::string_view{manifest}.substr(0, signature_offset(manifest))};
        expect_failure(fmt::format("signature = {{ scheme = \"ed25519\" }}\n{}",
                                   unsigned_manifest),
                       error_code::parse_error, "trailing [signature] table");
        expect_failure(
            replace_once(manifest, "scheme = \"ed25519\"", "scheme = \"rsa\""),
            error_code::invalid_argument,
            "unsupported signature "
            "scheme");
        expect_failure(replace_once(manifest, "key_id = \"refrain-test-key\"",
                                    "key_id = \"\""),
                       error_code::parse_error, "key_id must not be empty");
        expect_failure(replace_once(manifest, "sig = \"", "sig = \"Z"),
                       error_code::invalid_argument, "128 lowercase hex");
        // Bare container without [files]: rejected (not installable).
        {
            auto archive{archive_from(source_manifest(), files)};
            auto reader{open_archive(archive)};
            expect(static_cast<bool>(reader));
            if (!reader) {
                return;
            }
            auto verified{refrain::mod::verify_rfmod(*reader)};
            expect(!verified);
            expect(static_cast<int>(verified.error().code) ==
                   static_cast<int>(error_code::parse_error));
            expect(verified.error().message.find("[files]") !=
                   std::string::npos);
        }
    };
};

// ---------------------------------------------------------------------------
// install
// ---------------------------------------------------------------------------

const suite rfmod_signing_install_tests = [] {
    "rfmod/install publishes a trusted signed package and dedupes"_test = [] {
        temp_workspace work{"install-signed"};
        const auto keys{fixed_keypair()};
        materialise(work, "mod", default_tree(source_manifest()));
        rfmod_write_options write_options;
        write_options.signing = signing_for(test_key_id, keys);
        auto package{pack(work, "mod", "out/mod.rfmod", write_options)};
        expect(package.has_value());
        if (!package) {
            return;
        }
        auto reader{open_archive(package->archive)};
        expect(static_cast<bool>(reader));
        if (!reader) {
            return;
        }
        auto store{content_store::open(work.path("cas").string())};
        expect(static_cast<bool>(store));
        if (!store) {
            return;
        }
        const rfmod_install_options install_options{
            .require_signature = true,
            .trusted_keys = {trusted_for(test_key_id, keys)}};
        auto installed{
            refrain::mod::install_rfmod(*reader, *store, install_options)};
        expect(static_cast<bool>(installed)) << "trusted install failed";
        if (!installed) {
            return;
        }
        expect(installed->signed_package);
        expect(installed->trusted);
        expect(installed->key_id == test_key_id);
        expect(installed->stored_blobs == reader->file_count());
        expect(installed->new_blobs == reader->file_count());
        expect(installed->files.size() == reader->file_count());
        expect(installed->manifest.common.id == "refrain.test.signed");
        auto table{load_installed_blob_table(*store)};
        expect(static_cast<bool>(table)) << "blobs.idx was not published";
        if (!table) {
            return;
        }
        expect(table->entries.size() == installed->files.size());
        for (const auto &file : installed->files) {
            const auto *entry{find_table_entry(*table, file.digest)};
            expect(entry != nullptr) << "missing " << file.path;
            if (entry == nullptr) {
                continue;
            }
            expect((entry->flags & refrain::data::blob_flags_in_segment) != 0U)
                << file.path << " should live in a segment";
            auto stored{read_installed_blob(*store, *table, file.digest)};
            expect(static_cast<bool>(stored));
            expect(file.mode == 0644U);
        }
        auto manifest{
            read_installed_blob(*store, *table, installed->manifest_digest)};
        expect(static_cast<bool>(manifest));
        expect(count_files(work.path("cas/content/blobs")) == 0U)
            << "small members must not get standalone blob files";
        expect(count_segments(work.path("cas")) >= 1U)
            << "install published no content segment";

        // Re-install: same content, no new blobs.
        auto reinstalled{
            refrain::mod::install_rfmod(*reader, *store, install_options)};
        expect(static_cast<bool>(reinstalled));
        if (reinstalled) {
            expect(reinstalled->new_blobs == 0U);
            expect(reinstalled->stored_blobs == installed->stored_blobs);
            expect(digest_hex(reinstalled->manifest_digest) ==
                   digest_hex(installed->manifest_digest));
        }
    };

    "rfmod/install rejects an untrusted signature before writing blobs"_test =
        [] {
            temp_workspace work{"install-untrusted"};
            const auto keys{fixed_keypair()};
            materialise(work, "mod", default_tree(source_manifest()));
            rfmod_write_options write_options;
            write_options.signing = signing_for(test_key_id, keys);
            auto package{pack(work, "mod", "out/mod.rfmod", write_options)};
            expect(package.has_value());
            if (!package) {
                return;
            }
            auto reader{open_archive(package->archive)};
            expect(static_cast<bool>(reader));
            if (!reader) {
                return;
            }
            auto store{content_store::open(work.path("cas").string())};
            expect(static_cast<bool>(store));
            if (!store) {
                return;
            }
            const auto blobs_before{
                count_files(work.path("cas/content/blobs"))};
            expect(blobs_before == 0U);

            // Same key_id, wrong public key: verification fails.
            const auto result{refrain::mod::install_rfmod(
                *reader, *store,
                rfmod_install_options{.require_signature = true,
                                      .trusted_keys = {trusted_for(
                                          test_key_id, other_keypair())}})};
            expect(!result);
            expect(static_cast<int>(result.error().code) ==
                   static_cast<int>(error_code::parse_error));
            expect(count_files(work.path("cas/content/blobs")) == blobs_before)
                << "failed install must not publish blobs";

            // Unknown key_id with configured anchors: rejected, still no blob.
            const auto wrong_id{refrain::mod::install_rfmod(
                *reader, *store,
                rfmod_install_options{
                    .require_signature = true,
                    .trusted_keys = {trusted_for("other-key", keys)}})};
            expect(!wrong_id);
            expect(static_cast<int>(wrong_id.error().code) ==
                   static_cast<int>(error_code::not_found));
            expect(count_files(work.path("cas/content/blobs")) == blobs_before);
        };

    "rfmod/install accepts an unsigned package without require_signature"_test =
        [] {
            temp_workspace work{"install-unsigned"};
            materialise(work, "mod", default_tree(source_manifest()));
            auto package{pack(work, "mod", "out/mod.rfmod")};
            expect(package.has_value());
            if (!package) {
                return;
            }
            auto reader{open_archive(package->archive)};
            expect(static_cast<bool>(reader));
            if (!reader) {
                return;
            }
            auto store{content_store::open(work.path("cas").string())};
            expect(static_cast<bool>(store));
            if (!store) {
                return;
            }
            auto installed{refrain::mod::install_rfmod(*reader, *store)};
            expect(static_cast<bool>(installed));
            if (installed) {
                expect(!installed->signed_package);
                expect(!installed->trusted);
                expect(installed->key_id.empty());
                expect(installed->new_blobs == reader->file_count());
            }
        };
};

// ---------------------------------------------------------------------------
// Install receipts (the `files.json` equivalent)
// ---------------------------------------------------------------------------

const suite rfmod_signing_receipt_tests = [] {
    namespace json = refrain::data::json;

    "rfmod/install writes a deterministic signed receipt"_test = [] {
        temp_workspace work{"install-receipt-signed"};
        const auto keys{fixed_keypair()};
        materialise(work, "mod", default_tree(source_manifest()));
        rfmod_write_options write_options;
        write_options.signing = signing_for(test_key_id, keys);
        auto package{pack(work, "mod", "out/mod.rfmod", write_options)};
        expect(package.has_value());
        if (!package) {
            return;
        }
        auto reader{open_archive(package->archive)};
        expect(static_cast<bool>(reader));
        if (!reader) {
            return;
        }
        auto store{content_store::open(work.path("cas").string())};
        expect(static_cast<bool>(store));
        if (!store) {
            return;
        }
        const rfmod_install_options install_options{
            .require_signature = true,
            .trusted_keys = {trusted_for(test_key_id, keys)}};
        auto installed{
            refrain::mod::install_rfmod(*reader, *store, install_options)};
        expect(static_cast<bool>(installed)) << "trusted install failed";
        if (!installed) {
            return;
        }

        const auto expected_path{
            work.path("cas/mods/refrain.test.signed/1.0.0.json")};
        expect(installed->receipt_path == expected_path.string());
        expect(fs::is_regular_file(expected_path)) << "receipt file missing";

        auto document{json::value::parse_file(installed->receipt_path)};
        expect(static_cast<bool>(document)) << "receipt is not valid JSON";
        if (!document) {
            return;
        }
        const auto &receipt{*document};
        expect(receipt["schema"].as_string().value_or("") ==
               "refrain.mod-receipt/1");
        expect(receipt["id"].as_string().value_or("") == "refrain.test.signed");
        expect(receipt["version"].as_string().value_or("") == "1.0.0");
        expect(receipt["abi"].as_int64().value_or(-1) == 1);
        expect(receipt["archive_blake3"].as_string().value_or("") ==
               digest_hex(installed->archive_digest));
        expect(receipt["manifest_blake3"].as_string().value_or("") ==
               digest_hex(installed->manifest_digest));
        expect(receipt["signed"].as_bool().value_or(false));
        expect(receipt["trusted"].as_bool().value_or(false));
        expect(receipt["key_id"].as_string().value_or("") == test_key_id);
        expect(receipt["origin"].as_string().value_or("") == "user");

        auto files{receipt["files"].as_array()};
        expect(static_cast<bool>(files));
        if (!files) {
            return;
        }
        expect(files->size() == installed->files.size());
        expect(files->size() == 5U); // 4 content files + manifest.toml
        std::vector<std::string> paths;
        for (const auto &entry : *files) {
            const auto path{entry["path"].as_string().value_or("")};
            paths.emplace_back(path);
            expect(entry["mode"].as_int64().value_or(-1) == 420)
                << "recorded mode must be 0644";
            const auto found{std::find_if(installed->files.begin(),
                                          installed->files.end(),
                                          [&path](const installed_file &file) {
                                              return file.path == path;
                                          })};
            expect(found != installed->files.end())
                << "unexpected path " << path;
            if (found == installed->files.end()) {
                continue;
            }
            expect(entry["size"].as_int64().value_or(-1) ==
                   static_cast<std::int64_t>(found->size));
            expect(entry["blake3"].as_string().value_or("") ==
                   digest_hex(found->digest));
        }
        expect(std::is_sorted(paths.begin(), paths.end()));
        if (!paths.empty()) {
            expect(paths.front() == "LICENSE");
            expect(paths.back() == "scripts/init.wasm");
        }
        expect(std::find(paths.begin(), paths.end(), "manifest.toml") !=
               paths.end());

        // No host paths/timestamps: the receipt is reproducible.
        auto first_bytes{refrain::core::io::read_text(installed->receipt_path)};
        expect(static_cast<bool>(first_bytes));
        if (!first_bytes) {
            return;
        }
        expect(first_bytes->find(work.root.string()) == std::string::npos);

        auto reinstalled{
            refrain::mod::install_rfmod(*reader, *store, install_options)};
        expect(static_cast<bool>(reinstalled));
        if (!reinstalled) {
            return;
        }
        expect(reinstalled->receipt_path == installed->receipt_path);
        auto second_bytes{
            refrain::core::io::read_text(reinstalled->receipt_path)};
        expect(static_cast<bool>(second_bytes));
        if (second_bytes) {
            expect(*first_bytes == *second_bytes)
                << "reinstall must reproduce the receipt byte-for-byte";
        }
    };

    "rfmod/install receipt records an unsigned package"_test = [] {
        temp_workspace work{"install-receipt-unsigned"};
        materialise(work, "mod", default_tree(source_manifest()));
        auto package{pack(work, "mod", "out/mod.rfmod")};
        expect(package.has_value());
        if (!package) {
            return;
        }
        auto reader{open_archive(package->archive)};
        expect(static_cast<bool>(reader));
        if (!reader) {
            return;
        }
        auto store{content_store::open(work.path("cas").string())};
        expect(static_cast<bool>(store));
        if (!store) {
            return;
        }
        auto installed{refrain::mod::install_rfmod(*reader, *store)};
        expect(static_cast<bool>(installed));
        if (!installed) {
            return;
        }
        expect(installed->receipt_path ==
               work.path("cas/mods/refrain.test.signed/1.0.0.json").string());

        auto document{json::value::parse_file(installed->receipt_path)};
        expect(static_cast<bool>(document));
        if (!document) {
            return;
        }
        const auto &receipt{*document};
        expect(!receipt["signed"].as_bool().value_or(true));
        expect(!receipt["trusted"].as_bool().value_or(true));
        expect(receipt["key_id"].as_string().value_or("<missing>").empty());
        expect(receipt["archive_blake3"].as_string().value_or("") ==
               digest_hex(installed->archive_digest));
    };

    "rfmod/install accepts inner dots in a receipt path segment"_test = [] {
        temp_workspace work{"install-receipt-inner-dots"};
        // "a..b" is a valid mod id: the receipt-segment predicate must reject
        // only the exact ".." segment, not any occurrence of two dots.
        materialise(work, "mod",
                    default_tree(source_manifest("a..b", "1.0.0")));
        auto package{pack(work, "mod", "out/mod.rfmod")};
        expect(package.has_value());
        if (!package) {
            return;
        }
        auto reader{open_archive(package->archive)};
        expect(static_cast<bool>(reader));
        if (!reader) {
            return;
        }
        auto store{content_store::open(work.path("cas").string())};
        expect(static_cast<bool>(store));
        if (!store) {
            return;
        }
        auto installed{refrain::mod::install_rfmod(*reader, *store)};
        expect(static_cast<bool>(installed))
            << "a legitimate 'a..b' mod id was rejected";
        if (!installed) {
            return;
        }
        const auto expected{work.path("cas/mods/a..b/1.0.0.json")};
        expect(installed->receipt_path == expected.string());
        expect(fs::is_regular_file(expected)) << "receipt file missing";
    };

    "rfmod/install write_receipt=false publishes no receipt"_test = [] {
        temp_workspace work{"install-no-receipt"};
        materialise(work, "mod", default_tree(source_manifest()));
        auto package{pack(work, "mod", "out/mod.rfmod")};
        expect(package.has_value());
        if (!package) {
            return;
        }
        auto reader{open_archive(package->archive)};
        expect(static_cast<bool>(reader));
        if (!reader) {
            return;
        }
        auto store{content_store::open(work.path("cas").string())};
        expect(static_cast<bool>(store));
        if (!store) {
            return;
        }
        auto installed{refrain::mod::install_rfmod(
            *reader, *store, rfmod_install_options{.write_receipt = false})};
        expect(static_cast<bool>(installed));
        if (!installed) {
            return;
        }
        expect(installed->receipt_path.empty());
        expect(!fs::exists(work.path("cas/mods")));
        // Blobs are still stored: the receipt is a record, not a CAS gate.
        expect(installed->new_blobs == reader->file_count());
        auto table{load_installed_blob_table(*store)};
        expect(static_cast<bool>(table));
        if (table) {
            auto manifest{read_installed_blob(*store, *table,
                                              installed->manifest_digest)};
            expect(static_cast<bool>(manifest));
        }
    };

    "rfmod/install reports io_error when the receipt cannot be written"_test =
        [] {
            temp_workspace work{"install-receipt-io-error"};
            materialise(work, "mod", default_tree(source_manifest()));
            auto package{pack(work, "mod", "out/mod.rfmod")};
            expect(package.has_value());
            if (!package) {
                return;
            }
            auto reader{open_archive(package->archive)};
            expect(static_cast<bool>(reader));
            if (!reader) {
                return;
            }
            // A regular file where <store>/mods must be created makes the
            // receipt write fail after the batch committed and blobs.idx was
            // published.
            expect(work.write("cas/mods", "not a directory"));
            auto store{content_store::open(work.path("cas").string())};
            expect(static_cast<bool>(store));
            if (!store) {
                return;
            }
            const auto result{refrain::mod::install_rfmod(*reader, *store)};
            expect(!result);
            expect(static_cast<int>(result.error().code) ==
                   static_cast<int>(error_code::io_error));
            // Append-only: the committed segments and the published table stay.
            auto table{load_installed_blob_table(*store)};
            expect(static_cast<bool>(table))
                << "receipt failure must keep the committed blob table";
            if (table) {
                expect(table->entries.size() == reader->file_count());
                for (const auto &entry : reader->entries()) {
                    auto bytes{reader->read(entry.path)};
                    expect(static_cast<bool>(bytes));
                    if (!bytes) {
                        continue;
                    }
                    auto stored{read_installed_blob(
                        *store, *table, refrain::data::blake3(*bytes))};
                    expect(static_cast<bool>(stored))
                        << "receipt failure dropped " << entry.path;
                }
            }
            expect(count_segments(work.path("cas")) >= 1U)
                << "receipt failure must keep the sealed segments";
        };

    "rfmod/read_receipt exposes the installed receipt as a GC root"_test = [] {
        temp_workspace work{"read-receipt"};
        materialise(work, "mod", default_tree(source_manifest()));
        auto package{pack(work, "mod", "out/mod.rfmod")};
        expect(package.has_value());
        if (!package) {
            return;
        }
        auto reader{open_archive(package->archive)};
        expect(static_cast<bool>(reader));
        if (!reader) {
            return;
        }
        auto store{content_store::open(work.path("cas").string())};
        expect(static_cast<bool>(store));
        if (!store) {
            return;
        }
        auto installed{refrain::mod::install_rfmod(*reader, *store)};
        expect(static_cast<bool>(installed));
        if (!installed) {
            return;
        }
        auto receipt{refrain::mod::read_receipt(installed->receipt_path)};
        expect(static_cast<bool>(receipt)) << "read_receipt failed";
        if (!receipt) {
            return;
        }
        expect(receipt->id == "refrain.test.signed");
        expect(receipt->version == "1.0.0");
        expect(receipt->abi == 1U);
        expect(digest_hex(receipt->archive_blake3) ==
               digest_hex(installed->archive_digest));
        expect(digest_hex(receipt->manifest_blake3) ==
               digest_hex(installed->manifest_digest));
        expect(!receipt->signed_package);
        expect(!receipt->trusted);
        expect(receipt->key_id.empty());
        expect(receipt->origin == refrain::mod::mod_origin::user);
        expect(receipt->files.size() == installed->files.size());
        for (const auto &file : installed->files) {
            const auto found{
                std::find_if(receipt->files.begin(), receipt->files.end(),
                             [&file](const refrain::mod::receipt_file &entry) {
                                 return entry.path == file.path;
                             })};
            expect(found != receipt->files.end()) << "missing " << file.path;
            if (found != receipt->files.end()) {
                expect(found->size == file.size);
                expect(digest_hex(found->blake3) == digest_hex(file.digest));
            }
        }

        auto paths{refrain::mod::list_receipt_paths(work.path("cas").string())};
        expect(static_cast<bool>(paths));
        if (paths) {
            expect(paths->size() == 1U);
            if (!paths->empty()) {
                expect(paths->front() == installed->receipt_path);
            }
        }
        auto missing{
            refrain::mod::list_receipt_paths(work.path("empty").string())};
        expect(static_cast<bool>(missing));
        if (missing) {
            expect(missing->empty());
        }
    };

    "rfmod/read_receipt rejects corrupt and foreign receipts"_test = [] {
        temp_workspace work{"read-receipt-bad"};
        const auto relative{"mods/refrain.test.signed/1.0.0.json"};
        const auto path{work.path(relative)};
        expect(work.write(relative, "{"));
        const auto corrupt{refrain::mod::read_receipt(path.string())};
        expect(!corrupt);
        expect(static_cast<int>(corrupt.error().code) ==
               static_cast<int>(error_code::parse_error));
        expect(corrupt.error().message.find(path.string()) != std::string::npos)
            << "receipt error must name its path";

        expect(work.write(relative, R"({"schema":"refrain.mod-receipt/2"})"));
        const auto foreign{refrain::mod::read_receipt(path.string())};
        expect(!foreign);
        expect(static_cast<int>(foreign.error().code) ==
               static_cast<int>(error_code::parse_error));
        expect(foreign.error().message.find("schema") != std::string::npos);

        // Correct schema but missing required fields: strictly rejected.
        expect(work.write(relative, R"({"schema":"refrain.mod-receipt/1"})"));
        expect(!refrain::mod::read_receipt(path.string()));
    };
};

} // namespace
