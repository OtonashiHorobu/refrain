// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <refrain/core/error.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace refrain::app {

// Application service layer: command bodies shared by
// `refrain-tools` and `refrain-server`. The mains only parse CLI arguments and
// call one `run_*` service; no command implementation lives in a main.

[[nodiscard]] auto build_features() -> std::string;
void configure_logging(bool verbose);
[[nodiscard]] auto report_error(const refrain::error &error) -> int;

struct extract_options {
    std::string jar{};
    std::string out{"build/rfs"};
    std::string filter{"all"};
    std::string manifest_out{};
    bool extract{false};
};

struct vanilla_import_options {
    std::string reports{};
    std::string out{};
    std::string game_version{"26.3"};
    std::int32_t data_version{5023};
    bool force{false};
};

struct vanilla_info_options {
    std::string tables{};
};

struct protocol_bot_options {
    std::string target{};
    std::string username{"Bot"};
    int timeout_ms{10000};
    std::size_t wait_chunks{0};
    int linger_ms{0};
    std::string command{};
    std::size_t wait_messages{0};
    double move_dx{0.0};
    double move_dy{0.0};
    double move_dz{0.0};
    int move_delay_ms{0};
    int command_delay_ms{0};
};

struct protocol_load_options {
    std::string target{};
    std::string username_prefix{"Bot"};
    int bots{20};
    int duration_ms{10000};
    int timeout_ms{15000};
};

struct config_import_options {
    std::string properties{};
    std::string config_dir{"config"};
    bool force{false};
    bool strict{false};
};

struct mods_check_options {
    std::string mods_dir{"mods"};
    std::string gamepack{};
    bool strict{false};
};

struct mods_list_options {
    std::vector<std::string> roots{}; ///< instance roots (repeatable)
    std::string check_lock{};
    std::string gamepack{};
    std::string image{};
    bool strict{false};
};

struct data_options {
    std::vector<std::string> packs{}; ///< priority order (low -> high)
    std::uint32_t pack_format{0};     ///< 0 = accept any pack.mcmeta format
    bool structures{true};
    bool list{false}; ///< `data registry --list`: print every entry
};

struct data_cook_options {
    std::string pack{};
    std::string out{};
};

struct mods_load_options {
    std::string gamepack{"gamepack/gamepack.toml"};
    std::string image{}; ///< path to builtin.image (required)
    std::string side{"server"};
};

struct mods_pack_options {
    std::string mod_dir{};
    std::string out{};
    std::string seed_hex{};
    std::string key_id{};
    int level{3};
};

struct mods_verify_options {
    std::string archive{};
    std::string key_hex{};
    std::string key_id{};
    bool require_signature{false};
};

struct mods_install_options {
    /// Also write a raw RFS cook of the installed package under
    /// `<store>/content/cook/<id>-<version>.rfs` (default on).
    bool cook{true};
    std::string archive{};
    std::string store{};
    std::string key_hex{};
    std::string key_id{};
    bool require_signature{false};
};

struct mods_installed_options {
    std::string store{};
};

struct content_build_options {
    std::string root{};
    std::string store{};
    std::string source{};
    std::string composition{};
    int priority{0};
    std::string kind{"content"};
};

struct content_dump_options {
    std::string store{};
};

struct content_where_options {
    std::string store{};
    std::string composition{};
    std::string location{};
};

struct content_verify_options {
    std::string store{};
    bool blobs{false};
    bool strict{false};
};

struct content_gc_options {
    std::string store{};
    bool dry_run{false};
    bool apply{false};
    bool compact{false};
    int keep_days{0};
    std::uint64_t budget_ms{0};
};

struct builtin_image_options {
    std::string mods_dir{"mods"};
    std::string out{};
    std::string emit_cpp{};
};

[[nodiscard]] auto run_gamepack_dump(const std::string &path,
                                     std::string_view side) -> int;
[[nodiscard]] auto run_gamepack_check(const std::string &path,
                                      std::string_view side,
                                      const std::vector<std::string> &required)
    -> int;
[[nodiscard]] auto run_extract(const extract_options &options) -> int;
[[nodiscard]] auto run_vanilla_import(const vanilla_import_options &options)
    -> int;
[[nodiscard]] auto run_vanilla_info(const vanilla_info_options &options) -> int;
[[nodiscard]] auto
run_config_import_properties(const config_import_options &options) -> int;
[[nodiscard]] auto run_protocol_bot(const protocol_bot_options &options) -> int;
[[nodiscard]] auto run_protocol_load(const protocol_load_options &options)
    -> int;
[[nodiscard]] auto run_schema_gen(const std::string &output_root,
                                  const std::vector<std::string> &roots,
                                  bool check) -> int;
[[nodiscard]] auto run_crash_smoke(const std::string &database_path,
                                   const std::string &handler_path) -> int;
[[nodiscard]] auto run_wasm_smoke() -> int;
[[nodiscard]] auto run_mods_check(const mods_check_options &options) -> int;
[[nodiscard]] auto run_mods_list(const mods_list_options &options) -> int;
[[nodiscard]] auto run_mods_load(const mods_load_options &options) -> int;
[[nodiscard]] auto run_mods_pack(const mods_pack_options &options) -> int;
[[nodiscard]] auto run_mods_verify(const mods_verify_options &options) -> int;
[[nodiscard]] auto run_mods_install(const mods_install_options &options) -> int;
[[nodiscard]] auto run_mods_installed(const mods_installed_options &options)
    -> int;
[[nodiscard]] auto run_data(const data_options &options, bool validate) -> int;
[[nodiscard]] auto run_data_registry(const data_options &options) -> int;
[[nodiscard]] auto run_data_cook(const data_cook_options &options) -> int;
[[nodiscard]] auto run_content_build(const content_build_options &options)
    -> int;
[[nodiscard]] auto run_content_dump(const content_dump_options &options) -> int;
[[nodiscard]] auto run_content_where(const content_where_options &options)
    -> int;
[[nodiscard]] auto run_content_verify(const content_verify_options &options)
    -> int;
[[nodiscard]] auto run_content_gc(const content_gc_options &options) -> int;
[[nodiscard]] auto run_builtin_image(const builtin_image_options &options)
    -> int;

} // namespace refrain::app
