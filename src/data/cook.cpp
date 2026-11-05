// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/data/cook.hpp>

#include <refrain/core/io.hpp>
#include <refrain/data/json.hpp>
#include <refrain/data/nbt.hpp>
#include <refrain/data/path.hpp>

#include <algorithm>
#include <filesystem>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace refrain::data {
namespace {

namespace fs = std::filesystem;

[[nodiscard]] auto fail(error_code code, std::string message)
    -> std::unexpected<error> {
    return std::unexpected{make_error(code, std::move(message))};
}

} // namespace

auto cook_directory_rfs(std::string_view root, const rfs_write_options &options)
    -> result<cook_result> {
    if (root.empty()) {
        return fail(error_code::invalid_argument,
                    "cook: directory root must not be empty");
    }
    if (!core::io::is_directory(root)) {
        return fail(
            error_code::not_found,
            "cook: directory root does not exist or is not a directory: '" +
                std::string{root} + "'");
    }

    const fs::path root_path{std::string{root}};
    std::vector<rfs_input> records{};
    std::vector<std::vector<std::byte>> owners{};
    std::error_code iterate_error;
    fs::recursive_directory_iterator iterator{
        root_path, fs::directory_options::skip_permission_denied,
        iterate_error};
    if (iterate_error) {
        return fail(error_code::io_error, "cook: failed to enumerate '" +
                                              std::string{root} +
                                              "': " + iterate_error.message());
    }

    const fs::recursive_directory_iterator end{};
    while (iterator != end) {
        std::error_code status_error;
        const fs::file_status status{iterator->symlink_status(status_error)};
        if (status_error) {
            return fail(error_code::io_error,
                        "cook: failed to stat '" + iterator->path().string() +
                            "': " + status_error.message());
        }
        if (fs::is_symlink(status)) {
            iterator.disable_recursion_pending();
        } else if (fs::is_regular_file(status)) {
            const auto relative{iterator->path().lexically_relative(root_path)};
            const auto key{relative.generic_string()};
            if (!is_safe_relative_path(key)) {
                return fail(error_code::invalid_argument,
                            "cook: unsafe virtual path '" + key + "'");
            }
            auto bytes{core::io::read_file(iterator->path().string())};
            if (!bytes) {
                return std::unexpected{std::move(bytes.error())};
            }
            if (key.ends_with(".json")) {
                // Semantic slice: parse + re-encode canonical JSON so the RFS
                // record is validated and normalized.
                auto parsed{json::value::parse(*bytes)};
                if (!parsed) {
                    return fail(error_code::parse_error,
                                "cook: invalid JSON '" + key +
                                    "': " + parsed.error().message);
                }
                const auto normalized{parsed->dump()};
                owners.emplace_back(normalized.size());
                std::transform(
                    normalized.begin(), normalized.end(), owners.back().begin(),
                    [](char value) { return static_cast<std::byte>(value); });
            } else if (key.ends_with(".nbt")) {
                // Datapack structures are gzip-compressed NBT. Accept raw NBT
                // too, but always write the canonical gzip form.
                auto parsed{nbt::read(*bytes, nbt::compression::gzip)};
                if (!parsed) {
                    parsed = nbt::read(*bytes, nbt::compression::none);
                }
                if (!parsed) {
                    return fail(error_code::parse_error,
                                "cook: invalid NBT '" + key +
                                    "': " + parsed.error().message);
                }
                auto normalized{
                    nbt::write(*parsed, "", nbt::compression::gzip)};
                if (!normalized) {
                    return fail(error_code::parse_error,
                                "cook: cannot encode NBT '" + key +
                                    "': " + normalized.error().message);
                }
                owners.push_back(std::move(*normalized));
            } else {
                owners.push_back(std::move(*bytes));
            }
            records.push_back(rfs_input{
                .key = key, .data = std::span<const std::byte>{owners.back()}});
            if (records.size() > rfs_max_records) {
                return fail(error_code::parse_error,
                            "cook: directory has more than " +
                                std::to_string(rfs_max_records) + " files");
            }
        }

        iterator.increment(iterate_error);
        if (iterate_error) {
            return fail(error_code::io_error,
                        "cook: failed to enumerate '" + std::string{root} +
                            "': " + iterate_error.message());
        }
    }

    std::sort(records.begin(), records.end(),
              [](const rfs_input &lhs, const rfs_input &rhs) {
                  return lhs.key < rhs.key;
              });

    auto bytes{build_rfs(records, rfs_schema_hash{}, options)};
    if (!bytes) {
        return std::unexpected{std::move(bytes.error())};
    }
    return cook_result{.bytes = std::move(*bytes), .files = records.size()};
}

} // namespace refrain::data
