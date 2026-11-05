// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/archive/zip.hpp>

#include <refrain/core/io.hpp>

#include <fmt/format.h>
#include <zip.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace refrain::archive {

namespace {

[[nodiscard]] auto fail(error_code code, std::string message) -> error {
    return make_error(code, std::move(message));
}

/// Copy a libzip error message (`zip_error_t`) into our error taxonomy.
[[nodiscard]] auto zip_error(int code) -> error {
    zip_error_t error{};
    zip_error_init_with_code(&error, code);
    auto message{std::string{zip_error_strerror(&error)}};
    zip_error_fini(&error);
    return fail(error_code::io_error, std::move(message));
}

[[nodiscard]] auto archive_error(zip_t *archive) -> error {
    auto *cause{zip_get_error(archive)};
    return fail(error_code::io_error, zip_error_strerror(cause));
}

/// Per-file error state. `zip_fread()`/CRC failures are recorded on the
/// `zip_file_t`, not on the archive (`zip_get_error()` reports "No error" for
/// them), so diagnostics must read the file handle before it is closed.
[[nodiscard]] auto file_error_message(zip_file_t *file) -> std::string {
    return std::string{zip_error_strerror(zip_file_get_error(file))};
}

/// ASCII-only lowercase: archive names are byte strings, and a locale-aware
/// fold would make duplicate detection depend on the host locale.
[[nodiscard]] constexpr auto ascii_lower(char character) noexcept -> char {
    return character >= 'A' && character <= 'Z'
               ? static_cast<char>(character - 'A' + 'a')
               : character;
}

[[nodiscard]] constexpr auto equals_ascii_ci(std::string_view lhs,
                                             std::string_view rhs) noexcept
    -> bool {
    if (lhs.size() != rhs.size()) {
        return false;
    }
    for (std::size_t index{0}; index < lhs.size(); ++index) {
        if (ascii_lower(lhs[index]) != ascii_lower(rhs[index])) {
            return false;
        }
    }
    return true;
}

/// Windows reserves the DOS device names `CON`, `PRN`, `AUX`, `NUL`,
/// `COM1`-`COM9` and `LPT1`-`LPT9` *regardless of extension* ("con.txt" still
/// names the console device) and ignores trailing dots/spaces in file names.
/// Archives must extract portably, so names that would alias a device or be
/// silently rewritten on Windows are rejected on every host.
[[nodiscard]] constexpr auto
is_windows_reserved_name(std::string_view segment) noexcept -> bool {
    const auto dot{segment.find('.')};
    const auto base{segment.substr(0, dot)};
    if (base.empty() || base.size() > 4) {
        return false;
    }
    if (equals_ascii_ci(base, "con") || equals_ascii_ci(base, "prn") ||
        equals_ascii_ci(base, "aux") || equals_ascii_ci(base, "nul")) {
        return true;
    }
    if (base.size() == 4 && (equals_ascii_ci(base.substr(0, 3), "com") ||
                             equals_ascii_ci(base.substr(0, 3), "lpt"))) {
        return base[3] >= '1' && base[3] <= '9';
    }
    return false;
}

/// Trailing dots and spaces are stripped by Windows when the file is created,
/// so `"a."`/`"a "` would collide with `"a"` and the extracted name would not
/// match the archive entry.
[[nodiscard]] constexpr auto
has_windows_trailing_character(std::string_view segment) noexcept -> bool {
    return !segment.empty() && (segment.back() == '.' || segment.back() == ' ');
}

} // namespace

// ---------------------------------------------------------------------------
// Path safety
// ---------------------------------------------------------------------------

auto safe_relative_path(std::string_view name) noexcept -> bool {
    if (name.empty() || name.size() > 1024) {
        return false;
    }
    if (name.front() == '/' || name.find('\\') != std::string_view::npos ||
        name.find('\0') != std::string_view::npos) {
        return false;
    }
    // Windows drive letters / alternate data streams (":" is invalid in a
    // portable relative path).
    if (name.size() >= 2 && name[1] == ':') {
        return false;
    }

    std::size_t start{0};
    while (start < name.size()) {
        const auto slash{name.find('/', start)};
        const auto length{
            (slash == std::string_view::npos ? name.size() : slash) - start};
        const auto segment{name.substr(start, length)};
        if (segment.empty() || segment == "." || segment == "..") {
            return false;
        }
        // Portable-name rules (Windows device names, trailing dot/space
        // rewriting) are enforced on every host so an archive accepted here
        // extracts to the same names everywhere.
        if (is_windows_reserved_name(segment) ||
            has_windows_trailing_character(segment)) {
            return false;
        }
        for (const char character : segment) {
            const auto byte{static_cast<unsigned char>(character)};
            if (byte < 0x20U || character == ':' || character == '\\') {
                return false;
            }
        }
        if (slash == std::string_view::npos) {
            break;
        }
        start = slash + 1;
        if (start == name.size()) {
            return false; // trailing slash marks a directory: not a file path
        }
    }
    return true;
}

/// True when the output root or any existing path component below it is a
/// symlink. `extract()` refuses such a tree: a link could redirect a write
/// outside the extraction root (zip-slip by symlink). The check is
/// best-effort against TOCTOU, but it closes the persistent-link case without
/// changing the public path rules.
[[nodiscard]] auto has_symlink_component(std::string_view root,
                                         std::string_view relative) -> bool {
    if (core::io::is_symlink(root)) {
        return true;
    }
    std::string current{root};
    std::size_t start{0};
    while (start < relative.size()) {
        const auto slash{relative.find('/', start)};
        const auto length{
            (slash == std::string_view::npos ? relative.size() : slash) -
            start};
        current.push_back('/');
        current.append(relative.substr(start, length));
        if (core::io::is_symlink(current)) {
            return true;
        }
        if (slash == std::string_view::npos) {
            break;
        }
        start = slash + 1;
    }
    return false;
}

auto join_under(std::string_view root, std::string_view relative)
    -> std::string {
    auto joined{std::string{root}};
    if (!joined.empty() && joined.back() != '/') {
        joined.push_back('/');
    }
    joined.append(relative);
    return joined;
}

// ---------------------------------------------------------------------------
// zip_reader
// ---------------------------------------------------------------------------

struct zip_reader::impl {
    zip_t *archive{nullptr};
    std::string path{};
    std::vector<zip_entry> entries{};
    /// Exact-name index. Keys are views into `entries[i].name`; the vector
    /// (and therefore every name) is immutable after `open()`, so both the
    /// views and the `&entries[i]` pointers handed out by `find()` stay valid
    /// for the reader's lifetime. First occurrence wins for duplicate names.
    std::unordered_map<std::string_view, std::size_t> by_name{};

    ~impl() {
        if (archive != nullptr) {
            // Read-only handles are released with zip_discard(); zip_close()
            // would try to write the archive back.
            zip_discard(archive);
        }
    }
};

zip_reader::zip_reader(std::unique_ptr<impl> state) noexcept
    : impl_{std::move(state)} {}

zip_reader::zip_reader(zip_reader &&other) noexcept = default;

auto zip_reader::operator=(zip_reader &&other) noexcept
    -> zip_reader & = default;

zip_reader::~zip_reader() = default;

auto zip_reader::open(std::string_view path) -> result<zip_reader> {
    const auto native_path{std::string{path}};
    int error_code_value{0};
    auto *archive{zip_open(native_path.c_str(), ZIP_RDONLY, &error_code_value)};
    if (archive == nullptr) {
        return std::unexpected{zip_error(error_code_value)};
    }

    auto state{std::make_unique<impl>()};
    state->archive = archive;
    state->path = native_path;

    const auto count{zip_get_num_entries(archive, 0)};
    if (count < 0) {
        return std::unexpected{archive_error(archive)};
    }
    // The entry count is attacker-controlled archive metadata: reject it before
    // reserve() so a forged archive cannot make open() allocate an arbitrary
    // number of records. There is no >1M-entry fixture in the test suite (it
    // would be large and slow); the bound is part of the public contract
    // (`max_entries`) and checked here ahead of any per-entry allocation.
    if (static_cast<std::uint64_t>(count) > max_entries) {
        return std::unexpected{
            fail(error_code::parse_error,
                 fmt::format("zip: archive declares {} entries, above the "
                             "{}-entry safety limit",
                             count, max_entries))};
    }
    state->entries.reserve(static_cast<std::size_t>(count));
    for (zip_uint64_t index = 0; index < static_cast<zip_uint64_t>(count);
         ++index) {
        zip_stat_t stat{};
        zip_stat_init(&stat);
        if (zip_stat_index(archive, index, 0, &stat) != 0) {
            return std::unexpected{archive_error(archive)};
        }
        if ((stat.valid & ZIP_STAT_NAME) == 0 || stat.name == nullptr) {
            return std::unexpected{fail(error_code::parse_error,
                                        "zip: entry without a name at index " +
                                            std::to_string(index))};
        }
        zip_entry entry{};
        entry.name = stat.name;
        entry.index = index;
        entry.size = (stat.valid & ZIP_STAT_SIZE) != 0 ? stat.size : 0U;
        entry.compressed_size = (stat.valid & ZIP_STAT_COMP_SIZE) != 0
                                    ? stat.comp_size
                                    : entry.size;
        entry.crc32 = (stat.valid & ZIP_STAT_CRC) != 0
                          ? static_cast<std::uint32_t>(stat.crc)
                          : 0U;
        entry.method = (stat.valid & ZIP_STAT_COMP_METHOD) != 0
                           ? static_cast<std::uint16_t>(stat.comp_method)
                           : 0U;
        entry.directory = !entry.name.empty() && entry.name.back() == '/';
        state->entries.push_back(std::move(entry));
    }
    // Build the exact-name index once; lookups are then O(1) instead of a
    // linear scan (a tools' extract loop over a ~10k-entry manifest would be
    // O(n^2) otherwise).
    state->by_name.reserve(state->entries.size());
    for (std::size_t index{0}; index < state->entries.size(); ++index) {
        state->by_name.emplace(state->entries[index].name, index);
    }
    return zip_reader{std::move(state)};
}

auto zip_reader::archive_path() const noexcept -> std::string_view {
    return impl_ != nullptr ? std::string_view{impl_->path}
                            : std::string_view{};
}

auto zip_reader::entries() const noexcept -> const std::vector<zip_entry> & {
    static const std::vector<zip_entry> empty{};
    return impl_ != nullptr ? impl_->entries : empty;
}

auto zip_reader::find(std::string_view name) const noexcept
    -> const zip_entry * {
    if (impl_ == nullptr) {
        return nullptr;
    }
    const auto match{impl_->by_name.find(name)};
    if (match == impl_->by_name.end()) {
        return nullptr;
    }
    return &impl_->entries[match->second];
}

auto zip_reader::read(const zip_entry &entry)
    -> result<std::vector<std::byte>> {
    if (impl_ == nullptr || impl_->archive == nullptr) {
        return std::unexpected{
            fail(error_code::not_initialized, "zip: reader is not open")};
    }
    if (entry.directory) {
        return std::unexpected{
            fail(error_code::invalid_argument,
                 "zip: cannot read a directory entry '" + entry.name + "'")};
    }
    // The size is attacker-controlled archive metadata; reject oversized
    // declarations before allocating (zip bomb / OOM).
    if (entry.size > max_entry_bytes) {
        return std::unexpected{
            fail(error_code::parse_error,
                 "zip: entry '" + entry.name + "' declares " +
                     std::to_string(entry.size) + " bytes, above the " +
                     std::to_string(max_entry_bytes) + "-byte safety limit")};
    }
    if (entry.size > std::numeric_limits<std::size_t>::max()) {
        return std::unexpected{
            fail(error_code::parse_error,
                 "zip: entry '" + entry.name +
                     "' is too large to represent in memory on this platform")};
    }
    auto *file{zip_fopen_index(impl_->archive, entry.index, 0)};
    if (file == nullptr) {
        return std::unexpected{archive_error(impl_->archive)};
    }
    auto data{std::vector<std::byte>(static_cast<std::size_t>(entry.size))};
    std::size_t offset{0};
    while (offset < data.size()) {
        const auto remaining{static_cast<zip_uint64_t>(data.size() - offset)};
        const auto read{zip_fread(file, data.data() + offset, remaining)};
        if (read < 0) {
            const auto failure{file_error_message(file)};
            (void)zip_fclose(file);
            return std::unexpected{
                fail(error_code::io_error,
                     fmt::format("zip: failed to read entry '{}': {}",
                                 entry.name, failure))};
        }
        if (read == 0) {
            (void)zip_fclose(file);
            return std::unexpected{
                fail(error_code::io_error,
                     "zip: truncated entry '" + entry.name + "'")};
        }
        offset += static_cast<std::size_t>(read);
    }

    // libzip validates the entry CRC/size only when the stream reaches EOF.
    // Reading exactly `entry.size` bytes (and then closing) is not enough: the
    // declared size is untrusted metadata, and the CRC check would be skipped.
    // Probe one extra byte so either a corrupted entry (CRC error => negative)
    // or an entry larger than its declaration (extra byte => positive) fails.
    std::byte extra{};
    const auto probe{zip_fread(file, &extra, 1)};
    error probe_failure{};
    if (probe < 0) {
        // Copy the message before zip_fclose() releases the file handle.
        probe_failure = fail(error_code::io_error,
                             fmt::format("zip: failed to read entry '{}': {}",
                                         entry.name, file_error_message(file)));
    }
    const auto close_status{zip_fclose(file)};
    if (probe > 0) {
        return std::unexpected{
            fail(error_code::parse_error,
                 fmt::format("zip: entry '{}' is larger than its declared "
                             "{}-byte size",
                             entry.name, entry.size))};
    }
    if (probe < 0) {
        return std::unexpected{std::move(probe_failure)};
    }
    if (close_status != 0) {
        const auto close_failure{zip_error(close_status)};
        return std::unexpected{
            fail(error_code::io_error,
                 fmt::format("zip: failed to close entry '{}': {}", entry.name,
                             close_failure.message))};
    }
    return data;
}

auto zip_reader::read(std::string_view name) -> result<std::vector<std::byte>> {
    const auto *entry{find(name)};
    if (entry == nullptr) {
        return std::unexpected{
            fail(error_code::not_found,
                 "zip: entry '" + std::string{name} + "' not found")};
    }
    return read(*entry);
}

auto zip_reader::extract(const zip_entry &entry, std::string_view output_root)
    -> status {
    if (output_root.empty()) {
        return std::unexpected{fail(error_code::invalid_argument,
                                    "zip: output root must not be empty")};
    }
    if (!safe_relative_path(entry.name)) {
        return std::unexpected{
            fail(error_code::permission_denied,
                 "zip: refusing to extract unsafe path '" + entry.name + "'")};
    }
    if (has_symlink_component(output_root, entry.name)) {
        return std::unexpected{
            fail(error_code::permission_denied,
                 "zip: refusing to extract through a symlink for '" +
                     entry.name + "'")};
    }
    auto data{read(entry)};
    if (!data) {
        return std::unexpected{data.error()};
    }

    const auto output{join_under(output_root, entry.name)};
    const auto slash{output.find_last_of('/')};
    if (slash != std::string::npos && slash > 0) {
        const auto parent{output.substr(0, slash)};
        auto created{core::io::create_directories(parent)};
        if (!created) {
            return created;
        }
    }
    return core::io::atomic_write_file(output, *data);
}

} // namespace refrain::archive
