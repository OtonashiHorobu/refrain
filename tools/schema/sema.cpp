// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <schema/sema.hpp>

#include <schema/emitter_common.hpp>

#include <refrain/core/schedule.hpp>
#include <refrain/core/semver.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace refrain::schema {

auto primitive_name(primitive_kind value) noexcept -> std::string_view {
    switch (value) {
    case primitive_kind::u8:
        return "u8";
    case primitive_kind::u16:
        return "u16";
    case primitive_kind::u32:
        return "u32";
    case primitive_kind::u64:
        return "u64";
    case primitive_kind::i8:
        return "i8";
    case primitive_kind::i16:
        return "i16";
    case primitive_kind::i32:
        return "i32";
    case primitive_kind::i64:
        return "i64";
    case primitive_kind::f32:
        return "f32";
    case primitive_kind::f64:
        return "f64";
    case primitive_kind::boolean:
        return "bool";
    case primitive_kind::string:
        return "string";
    case primitive_kind::bytes:
        return "bytes";
    case primitive_kind::identifier:
        return "identifier";
    case primitive_kind::uuid:
        return "uuid";
    case primitive_kind::text_component:
        return "text_component";
    case primitive_kind::item_stack:
        return "item_stack";
    case primitive_kind::lp_vec3:
        return "lp_vec3";
    case primitive_kind::nbt:
        return "nbt";
    case primitive_kind::varint_id:
        return "varint_id";
    case primitive_kind::message_signature_packed:
        return "message_signature_packed";
    case primitive_kind::filter_mask:
        return "filter_mask";
    case primitive_kind::stat_entry:
        return "stat_entry";
    case primitive_kind::position_path:
        return "position_path";
    case primitive_kind::map_patch:
        return "map_patch";
    case primitive_kind::advancement_seen:
        return "advancement_seen";
    case primitive_kind::vec_delta:
        return "vec_delta";
    case primitive_kind::sound_selector:
        return "sound_selector";
    case primitive_kind::objective_update:
        return "objective_update";
    case primitive_kind::team_update:
        return "team_update";
    case primitive_kind::player_info_update:
        return "player_info_update";
    case primitive_kind::tracked_waypoint:
        return "tracked_waypoint";
    case primitive_kind::void_type:
        return "void";
    }
    return "?";
}

auto primitive_from_name(std::string_view name) noexcept
    -> std::optional<primitive_kind> {
    static constexpr std::array table{
        std::pair{"u8", primitive_kind::u8},
        std::pair{"u16", primitive_kind::u16},
        std::pair{"u32", primitive_kind::u32},
        std::pair{"u64", primitive_kind::u64},
        std::pair{"i8", primitive_kind::i8},
        std::pair{"i16", primitive_kind::i16},
        std::pair{"i32", primitive_kind::i32},
        std::pair{"i64", primitive_kind::i64},
        std::pair{"f32", primitive_kind::f32},
        std::pair{"f64", primitive_kind::f64},
        std::pair{"bool", primitive_kind::boolean},
        std::pair{"string", primitive_kind::string},
        std::pair{"bytes", primitive_kind::bytes},
        std::pair{"identifier", primitive_kind::identifier},
        std::pair{"uuid", primitive_kind::uuid},
        std::pair{"text_component", primitive_kind::text_component},
        std::pair{"item_stack", primitive_kind::item_stack},
        std::pair{"lp_vec3", primitive_kind::lp_vec3},
        std::pair{"nbt", primitive_kind::nbt},
        std::pair{"varint_id", primitive_kind::varint_id},
        std::pair{"message_signature_packed",
                  primitive_kind::message_signature_packed},
        std::pair{"filter_mask", primitive_kind::filter_mask},
        std::pair{"stat_entry", primitive_kind::stat_entry},
        std::pair{"position_path", primitive_kind::position_path},
        std::pair{"map_patch", primitive_kind::map_patch},
        std::pair{"advancement_seen", primitive_kind::advancement_seen},
        std::pair{"vec_delta", primitive_kind::vec_delta},
        std::pair{"sound_selector", primitive_kind::sound_selector},
        std::pair{"objective_update", primitive_kind::objective_update},
        std::pair{"team_update", primitive_kind::team_update},
        std::pair{"player_info_update", primitive_kind::player_info_update},
        std::pair{"tracked_waypoint", primitive_kind::tracked_waypoint},
        std::pair{"void", primitive_kind::void_type},
    };
    for (const auto &[key, value] : table) {
        if (key == name) {
            return value;
        }
    }
    return std::nullopt;
}

namespace {

struct size_align {
    std::uint32_t size{0};
    std::uint32_t alignment{1};
};

struct decl_ref {
    std::uint32_t module{0};
    std::uint32_t decl{0};

    friend auto operator<=>(const decl_ref &, const decl_ref &) = default;
};

struct integer_info {
    unsigned bits{0};
    bool is_signed{false};
};

[[nodiscard]] constexpr auto align_up(std::uint64_t value,
                                      std::uint64_t alignment) noexcept
    -> std::uint64_t {
    return (value + alignment - 1) / alignment * alignment;
}

[[nodiscard]] auto integer_info_of(primitive_kind kind) noexcept
    -> std::optional<integer_info> {
    switch (kind) {
    case primitive_kind::u8:
        return integer_info{8, false};
    case primitive_kind::u16:
        return integer_info{16, false};
    case primitive_kind::u32:
        return integer_info{32, false};
    case primitive_kind::u64:
        return integer_info{64, false};
    case primitive_kind::i8:
        return integer_info{8, true};
    case primitive_kind::i16:
        return integer_info{16, true};
    case primitive_kind::i32:
        return integer_info{32, true};
    case primitive_kind::i64:
        return integer_info{64, true};
    default:
        return std::nullopt;
    }
}

[[nodiscard]] auto integer_info_of(std::string_view type_name) noexcept
    -> std::optional<integer_info> {
    const auto primitive{primitive_from_name(type_name)};
    return primitive.has_value() ? integer_info_of(*primitive) : std::nullopt;
}

[[nodiscard]] auto fits_integer(std::int64_t value, integer_info info) noexcept
    -> bool {
    if (info.is_signed) {
        if (info.bits >= 64) {
            return true;
        }
        const auto limit{static_cast<std::int64_t>(1) << (info.bits - 1)};
        return value >= -limit && value < limit;
    }
    if (value < 0) {
        return false;
    }
    if (info.bits >= 64) {
        return true;
    }
    return static_cast<std::uint64_t>(value) <
           (static_cast<std::uint64_t>(1) << info.bits);
}

[[nodiscard]] auto is_known_attribute(std::string_view name) noexcept -> bool {
    static constexpr std::array names{
        "since",        "deprecated",
        "removed",      "breaking",
        "default",      "unit",
        "max",          "varint",
        "varlong",      "fixed",
        "fixed_bytes",  "optional_varint",
        "rest",         "fixed_count",
        "zigzag",       "bits",
        "sidecar",      "side",
        "thread",       "borrow",
        "owned",        "caller_provided",
        "can_fail",     "hot_path",
        "sandbox_safe", "network_sync",
        "on_load_only", "batch",
        "cancellable",  "schedule",
    };
    return std::ranges::any_of(names, [name](std::string_view candidate) {
        return candidate == name;
    });
}

/// Where an attribute appeared. Attributes are validated in context: `#[batch]`
/// is only meaningful on an event declaration, `#[schedule]` only on a service
/// method, and the same names on a struct/field are hard errors.
enum class attribute_scope : std::uint8_t {
    declaration,
    field,
    method,
};

[[nodiscard]] auto find_attribute(const std::vector<attribute> &attributes,
                                  std::string_view name) noexcept
    -> const attribute * {
    const auto found{
        std::ranges::find_if(attributes, [name](const attribute &candidate) {
            return candidate.name == name;
        })};
    return found == attributes.end() ? nullptr : &*found;
}

/// Scalar types allowed as event fields and as `slice<T>` elements of an event
/// (string/bytes are event fields but not scalars).
[[nodiscard]] constexpr auto is_scalar_primitive(primitive_kind kind) noexcept
    -> bool {
    switch (kind) {
    case primitive_kind::u8:
    case primitive_kind::u16:
    case primitive_kind::u32:
    case primitive_kind::u64:
    case primitive_kind::i8:
    case primitive_kind::i16:
    case primitive_kind::i32:
    case primitive_kind::i64:
    case primitive_kind::f32:
    case primitive_kind::f64:
    case primitive_kind::boolean:
        return true;
    case primitive_kind::string:
    case primitive_kind::bytes:
    case primitive_kind::identifier:
    case primitive_kind::uuid:
    case primitive_kind::text_component:
    case primitive_kind::item_stack:
    case primitive_kind::lp_vec3:
    case primitive_kind::nbt:
    case primitive_kind::varint_id:
    case primitive_kind::message_signature_packed:
    case primitive_kind::filter_mask:
    case primitive_kind::stat_entry:
    case primitive_kind::position_path:
    case primitive_kind::map_patch:
    case primitive_kind::advancement_seen:
    case primitive_kind::vec_delta:
    case primitive_kind::sound_selector:
    case primitive_kind::objective_update:
    case primitive_kind::team_update:
    case primitive_kind::player_info_update:
    case primitive_kind::tracked_waypoint:
    case primitive_kind::void_type:
        return false;
    }
    return false;
}

[[nodiscard]] constexpr auto is_integer_primitive(primitive_kind kind) noexcept
    -> bool {
    switch (kind) {
    case primitive_kind::u8:
    case primitive_kind::u16:
    case primitive_kind::u32:
    case primitive_kind::u64:
    case primitive_kind::i8:
    case primitive_kind::i16:
    case primitive_kind::i32:
    case primitive_kind::i64:
        return true;
    case primitive_kind::f32:
    case primitive_kind::f64:
    case primitive_kind::boolean:
    case primitive_kind::string:
    case primitive_kind::bytes:
    case primitive_kind::identifier:
    case primitive_kind::uuid:
    case primitive_kind::text_component:
    case primitive_kind::item_stack:
    case primitive_kind::lp_vec3:
    case primitive_kind::nbt:
    case primitive_kind::varint_id:
    case primitive_kind::message_signature_packed:
    case primitive_kind::filter_mask:
    case primitive_kind::stat_entry:
    case primitive_kind::position_path:
    case primitive_kind::map_patch:
    case primitive_kind::advancement_seen:
    case primitive_kind::vec_delta:
    case primitive_kind::sound_selector:
    case primitive_kind::objective_update:
    case primitive_kind::team_update:
    case primitive_kind::player_info_update:
    case primitive_kind::tracked_waypoint:
    case primitive_kind::void_type:
        return false;
    }
    return false;
}

[[nodiscard]] auto all_digits(std::string_view text) noexcept -> bool {
    return !text.empty() && std::ranges::all_of(text, [](char value) {
        return value >= '0' && value <= '9';
    });
}

/// Version component: decimal digits, no leading zeros (mirrors the gamepack
/// semver rules: `007` is rejected, `0` is fine), so both dialects agree.
[[nodiscard]] auto valid_version_component(std::string_view text) noexcept
    -> bool {
    if (!all_digits(text)) {
        return false;
    }
    return text.size() == 1 || text.front() != '0';
}

/// RSC data-schema module version: a single decimal component (`version 2`).
/// Data/registry schemas version their own formats and are outside the
/// contract SemVer space; RIDL contracts require a full SemVer triple
/// instead. This keeps the RSC rule (no `major.minor`).
[[nodiscard]] auto valid_data_version_text(std::string_view text) noexcept
    -> bool {
    return valid_version_component(text);
}

/// Render a `refrain::error` (from the shared core semver parser) as a compact,
/// readable suffix for a schema diagnostic.
[[nodiscard]] auto error_text(const refrain::error &failure) -> std::string {
    return failure.message;
}

class analyzer {
  public:
    analyzer(std::vector<source_unit> units, diagnostics &sink)
        : units_{std::move(units)}, sink_{sink} {}

    [[nodiscard]] auto run() -> std::optional<model> {
        const auto baseline{sink_.size()};
        build_modules();
        build_symbol_tables();
        resolve_declarations();
        compute_abi();
        if (sink_.size() != baseline) {
            return std::nullopt;
        }
        return std::move(model_);
    }

  private:
    [[nodiscard]] auto qualified(std::uint32_t module_index,
                                 std::string_view decl_name) const
        -> std::string {
        return model_.modules[module_index].name +
               "::" + std::string{decl_name};
    }

    [[nodiscard]] auto module_at(std::uint32_t index) const
        -> const module_model & {
        return model_.modules[index];
    }

    [[nodiscard]] auto decl_at(decl_ref ref) const -> const decl_model & {
        return model_.modules[ref.module].declarations[ref.decl];
    }

    void error(const source_location &where, std::string message) {
        sink_.error(where, std::move(message));
    }

    // -- phase 1: group files into modules --------------------------------
    void build_modules() {
        std::sort(units_.begin(), units_.end(),
                  [](const source_unit &lhs, const source_unit &rhs) {
                      return lhs.path < rhs.path;
                  });

        std::map<std::string, std::vector<const source_unit *>> grouped;
        for (const auto &unit : units_) {
            grouped[unit.module].push_back(&unit);
        }

        std::map<std::string, std::string> abi_prefixes;
        for (const auto &[module_name, files] : grouped) {
            module_model module;
            module.name = module_name;
            module.kind = files.front()->kind;
            module.version = files.front()->version;
            const source_location module_location{files.front()->path, 1, 1};
            // Module names must be rooted at `refrain` and have at least
            // two segments; otherwise `module_rel_path` degenerates (empty file
            // name, hidden `.hpp`) and the generated guard contains reserved
            // double underscores.
            const auto segments{emit::split_module(module_name)};
            if (segments.size() < 2) {
                error(module_location,
                      std::format("module '{}' must have at least two segments "
                                  "(e.g. 'refrain::services::world'); a bare "
                                  "root module would generate a degenerate "
                                  "file path",
                                  module_name));
            } else if (segments.front() != "refrain") {
                error(module_location,
                      std::format("module '{}' must be rooted at 'refrain' "
                                  "(generated namespaces and paths are rooted "
                                  "there)",
                                  module_name));
            }
            // Distinct modules must not collapse to the same C ABI prefix
            // (`a::b_c` and `a::b::c` both become `a_b_c`), otherwise two
            // generated headers define colliding C symbols.
            const auto prefix{emit::abi_prefix(module_name)};
            if (const auto existing{abi_prefixes.find(prefix)};
                existing != abi_prefixes.end() &&
                existing->second != module_name) {
                error(
                    module_location,
                    std::format("modules '{}' and '{}' share the C ABI prefix "
                                "'{}'; rename one of them",
                                existing->second, module_name, prefix));
            } else {
                abi_prefixes.emplace(prefix, module_name);
            }
            if (module.kind == dialect::ridl) {
                // RIDL contract modules declare a full SemVer 2.0.0 version;
                // prerelease/build metadata is syntactically valid
                // for the module itself (service versions have stricter rules).
                const auto parsed_version{core::semver::parse(module.version)};
                if (!parsed_version.has_value()) {
                    error(module_location,
                          std::format("module '{}' has invalid version '{}': "
                                      "expected a full SemVer 2.0.0 version "
                                      "major.minor.patch ({})",
                                      module_name, module.version,
                                      error_text(parsed_version.error())));
                }
            } else if (!valid_data_version_text(module.version)) {
                error(module_location,
                      std::format("module '{}' has invalid version '{}'",
                                  module_name, module.version));
            }
            for (const auto *file : files) {
                if (file->kind != module.kind) {
                    error({file->path, 1, 1},
                          std::format("module '{}' is declared as both RSC and "
                                      "RIDL in the same build",
                                      module_name));
                }
                if (file->version != module.version) {
                    error({file->path, 1, 1},
                          std::format("module '{}' has version '{}' here but "
                                      "'{}' in another file",
                                      module_name, file->version,
                                      module.version));
                }
                for (const auto &imported : file->imports) {
                    if (std::ranges::find(module.imports, imported) ==
                        module.imports.end()) {
                        module.imports.push_back(imported);
                    }
                }
                for (const auto &declaration : file->declarations) {
                    module.declarations.push_back(
                        make_decl_model(declaration, file->path));
                }
            }
            std::size_t service_count{0};
            for (const auto &declaration : module.declarations) {
                if (declaration.kind == declaration_kind::service) {
                    ++service_count;
                }
            }
            if (service_count > 1) {
                error(module_location,
                      std::format("module '{}' declares {} services; ABI v0 "
                                  "supports one service per module (generated "
                                  "type names would collide)",
                                  module_name, service_count));
            }
            model_.modules.push_back(std::move(module));
        }

        // AST pointers per module, aligned 1:1 with model declarations.
        ast_declarations_.resize(model_.modules.size());
        for (std::uint32_t module_index = 0;
             module_index < model_.modules.size(); ++module_index) {
            auto &target{ast_declarations_[module_index]};
            for (const auto *file :
                 grouped[model_.modules[module_index].name]) {
                for (const auto &declaration : file->declarations) {
                    target.push_back(&declaration);
                }
            }
        }
    }

    [[nodiscard]] static auto make_decl_model(const declaration &source,
                                              std::string_view source_path)
        -> decl_model {
        decl_model decl;
        decl.kind = source.kind;
        decl.name = source.name;
        decl.source_path = std::string{source_path};
        decl.documentation = source.documentation;
        decl.attributes = source.attributes;
        decl.location = source.location;
        decl.packet_id = source.packet_id;
        decl.underlying = source.underlying;
        decl.entries = source.entries;
        decl.service = source.service;
        decl.service_version = source.service.version;
        return decl;
    }

    // -- phase 2: symbol tables -------------------------------------------
    void build_symbol_tables() {
        std::set<std::string> module_names;
        for (const auto &module : model_.modules) {
            module_names.insert(module.name);
        }
        for (const auto &module : model_.modules) {
            for (const auto &imported : module.imports) {
                if (!module_names.contains(imported)) {
                    error({module.name, 1, 1},
                          std::format("module '{}' imports unknown module '{}'",
                                      module.name, imported));
                }
            }
        }
        for (std::uint32_t module_index = 0;
             module_index < model_.modules.size(); ++module_index) {
            const auto &module{module_at(module_index)};
            for (std::uint32_t decl_index = 0;
                 decl_index < module.declarations.size(); ++decl_index) {
                const auto &decl{module.declarations[decl_index]};
                const auto key{qualified(module_index, decl.name)};
                if (symbols_.contains(key)) {
                    error(decl.location,
                          std::format("duplicate declaration '{}' in module "
                                      "'{}'",
                                      decl.name, module.name));
                } else {
                    symbols_.emplace(key, decl_ref{module_index, decl_index});
                }
                if (decl.kind == declaration_kind::handle) {
                    if (handles_.contains(decl.name)) {
                        error(decl.location,
                              std::format("handle tag '{}' is declared in "
                                          "more than one module",
                                          decl.name));
                    } else {
                        handles_.emplace(decl.name,
                                         decl_ref{module_index, decl_index});
                    }
                }
            }
        }
    }

    /// Resolve a named type: fully qualified, local, or imported (imports must
    /// be unambiguous). Emits a diagnostic for qualified misses/ambiguities.
    [[nodiscard]] auto find_named(std::string_view name,
                                  std::uint32_t current_module,
                                  const source_location &where)
        -> std::optional<decl_ref> {
        const auto search =
            [this](const std::string &key) -> std::optional<decl_ref> {
            const auto found{symbols_.find(key)};
            if (found != symbols_.end()) {
                return found->second;
            }
            return std::nullopt;
        };
        if (name.find("::") != std::string_view::npos) {
            const auto found{search(std::string{name})};
            if (!found.has_value()) {
                error(where, std::format("unknown type '{}'", name));
            }
            return found;
        }
        const auto &module{module_at(current_module)};
        if (auto local{search(module.name + "::" + std::string{name})};
            local.has_value()) {
            return local;
        }
        std::vector<decl_ref> matches;
        for (const auto &imported : module.imports) {
            if (auto found{search(imported + "::" + std::string{name})};
                found.has_value()) {
                matches.push_back(*found);
            }
        }
        if (matches.size() == 1) {
            return matches.front();
        }
        if (matches.size() > 1) {
            error(where, std::format("ambiguous type '{}' (imported from "
                                     "multiple modules)",
                                     name));
            return std::nullopt;
        }
        return std::nullopt;
    }

    [[nodiscard]] auto find_handle(std::string_view tag) const
        -> std::optional<decl_ref> {
        const auto found{handles_.find(std::string{tag})};
        if (found != handles_.end()) {
            return found->second;
        }
        return std::nullopt;
    }

    // -- phase 3: types, declarations, services ---------------------------
    void resolve_declarations() {
        for (std::uint32_t module_index = 0;
             module_index < model_.modules.size(); ++module_index) {
            auto &module{model_.modules[module_index]};
            for (std::uint32_t decl_index = 0;
                 decl_index < module.declarations.size(); ++decl_index) {
                auto &decl{module.declarations[decl_index]};
                if (decl_index < ast_declarations_[module_index].size()) {
                    resolve_declaration(
                        module_index, decl,
                        *ast_declarations_[module_index][decl_index]);
                }
            }
        }
    }

    void resolve_declaration(std::uint32_t module_index, decl_model &decl,
                             const declaration &source) {
        validate_attributes(decl.attributes, decl.location,
                            attribute_scope::declaration, decl.kind);
        const auto ridl{module_at(module_index).kind == dialect::ridl};

        switch (decl.kind) {
        case declaration_kind::structure:
        case declaration_kind::record:
        case declaration_kind::packet:
            if (ridl && decl.kind != declaration_kind::structure) {
                error(decl.location,
                      std::format("'{}' is only valid in RSC schemas",
                                  declaration_kind_name(decl.kind)));
            }
            resolve_struct_fields(module_index, decl);
            if (decl.kind == declaration_kind::packet) {
                validate_packet(module_index, decl);
                validate_packet_module(module_index, decl);
            }
            break;
        case declaration_kind::enumeration:
        case declaration_kind::flags:
            if ((!ridl && decl.kind == declaration_kind::flags)) {
                error(decl.location, "'flags' is only valid in RIDL schemas");
            }
            resolve_enum(decl);
            break;
        case declaration_kind::handle:
            if (!ridl) {
                error(decl.location,
                      "'handle' is only valid in RIDL service contracts");
            }
            break;
        case declaration_kind::service:
            if (!ridl) {
                error(decl.location,
                      "'service' is only valid in RIDL service contracts");
            }
            resolve_service(module_index, decl, source);
            break;
        case declaration_kind::event:
            // Events are in-process notifications: they have no C ABI, no
            // persistence and no wire format, so they only exist in RIDL.
            if (!ridl) {
                error(decl.location,
                      "event is a RIDL declaration; events are in-process "
                      "notifications and do not exist in RSC data schemas");
                break;
            }
            // A payload-less event has no well-defined layout: the padding
            // algorithm yields size 0 while a C++ empty struct is one byte, so
            // the generated `static_assert(sizeof(...) == 0)` could not hold.
            // Fail closed instead of inventing an ABI.
            if (source.fields.empty()) {
                error(decl.location,
                      std::format("event '{}' must declare at least one "
                                  "field; a payload-less event has no defined "
                                  "layout",
                                  decl.name));
                break;
            }
            resolve_event_fields(module_index, decl);
            validate_event_attributes(decl);
            break;
        }
    }

    void resolve_struct_fields(std::uint32_t module_index, decl_model &decl) {
        const auto ridl{module_at(module_index).kind == dialect::ridl};
        const auto &source{*ast_declarations_[module_index][decl_index_of(
            module_index, decl)]};
        std::set<std::string> field_names;
        for (const auto &field_source : source.fields) {
            auto field{resolve_field(field_source, module_index)};
            if (!field.has_value()) {
                continue;
            }
            // Duplicate field names would silently produce C structs with
            // duplicate members (compile error) and wrong static_assert
            // layout values. Reject the declaration instead.
            if (!field_names.insert(field->name).second) {
                error(field->location,
                      std::format("duplicate field '{}' in {} '{}'",
                                  field->name, declaration_kind_name(decl.kind),
                                  decl.name));
                continue;
            }
            if (ridl) {
                if (!is_valid_contract_field_type(field->type)) {
                    error(field->location,
                          std::format("field '{}' has a type that is not "
                                      "allowed in a contract struct (use "
                                      "primitives, string, bytes, "
                                      "slice<struct>, handles, enums, flags or "
                                      "structs)",
                                      field->name));
                    continue;
                }
            } else {
                if (field->type.kind == resolved_type::category::handle) {
                    error(field->location,
                          "handles are not allowed in RSC data schemas");
                }
                if (!is_valid_data_field_type(field->type)) {
                    error(field->location,
                          std::format("field '{}' references a packet or "
                                      "handle type, which cannot be used as "
                                      "data",
                                      field->name));
                    continue;
                }
            }
            decl.fields.push_back(std::move(*field));
        }
    }

    // -- events -------------------------------------------------------------
    void resolve_event_fields(std::uint32_t module_index, decl_model &decl) {
        const auto &source{*ast_declarations_[module_index][decl_index_of(
            module_index, decl)]};
        std::set<std::string> field_names;
        for (const auto &field_source : source.fields) {
            auto field{resolve_field(field_source, module_index)};
            if (!field.has_value()) {
                continue;
            }
            if (!field_names.insert(field->name).second) {
                error(field->location,
                      std::format("duplicate field '{}' in event '{}'",
                                  field->name, decl.name));
                continue;
            }
            if (field_source.default_value.has_value()) {
                error(field->location,
                      std::format("event field '{}' cannot have a default "
                                  "value; events carry only the published "
                                  "payload",
                                  field->name));
                continue;
            }
            if (find_attribute(field->attributes, "default") != nullptr) {
                error(field->location,
                      std::format("event field '{}' cannot use '#[default]'; "
                                  "events carry only the published payload",
                                  field->name));
                continue;
            }
            if (const auto rejection{
                    event_field_type_error(field->name, field->type)};
                rejection.has_value()) {
                error(field->location, *rejection);
                continue;
            }
            decl.fields.push_back(std::move(*field));
        }
    }

    /// Event field whitelist: scalars, string, bytes and `slice<scalar>`. The
    /// error text names the offending type so a contract struct/array/handle
    /// mix-up is diagnosed precisely.
    [[nodiscard]] auto event_field_type_error(std::string_view field_name,
                                              const resolved_type &type) const
        -> std::optional<std::string> {
        static constexpr std::string_view allowed{
            "events only accept scalar (i8..i64, u8..u64, bool, f32, f64), "
            "string, bytes or slice<scalar> fields"};
        switch (type.kind) {
        case resolved_type::category::primitive:
            if (type.primitive == primitive_kind::void_type) {
                return std::format("event field '{}' cannot be 'void'; {}",
                                   field_name, allowed);
            }
            return std::nullopt;
        case resolved_type::category::slice: {
            const auto scalar{
                type.arguments.size() == 1 &&
                type.arguments.front().kind ==
                    resolved_type::category::primitive &&
                is_scalar_primitive(type.arguments.front().primitive)};
            if (!scalar) {
                return std::format(
                    "event field '{}' must be 'slice<scalar>' whose element "
                    "is i8..i64, u8..u64, bool, f32 or f64 (string, bytes, "
                    "struct and nested slice elements are not allowed)",
                    field_name);
            }
            return std::nullopt;
        }
        case resolved_type::category::named: {
            const auto &target{decl_at({type.module_index, type.decl_index})};
            return std::format(
                "event field '{}' references {} '{}', but events cannot "
                "reference named contract types (no struct_size-bearing "
                "structs, enums, flags or handles); {}",
                field_name, declaration_kind_name(target.kind), target.name,
                allowed);
        }
        case resolved_type::category::handle:
            return std::format(
                "event field '{}' is a handle; handles are opaque host-side "
                "resources and cannot be copied into an in-process event; {}",
                field_name, allowed);
        case resolved_type::category::array:
            return std::format(
                "event field '{}' uses 'array<T>', which has no length "
                "parameter in RIDL; use 'slice<scalar>' to borrow a "
                "publish-scoped span; {}",
                field_name, allowed);
        case resolved_type::category::optional:
            return std::format("event field '{}' uses 'optional<T>', which is "
                               "not representable in an event payload; {}",
                               field_name, allowed);
        case resolved_type::category::repeated:
            return std::format("event field '{}' uses 'repeated<T>', which is "
                               "not representable in an event payload; {}",
                               field_name, allowed);
        case resolved_type::category::map:
            return std::format(
                "event field '{}' uses 'map<K, V>', which is not "
                "representable in an event payload; {}",
                field_name, allowed);
        case resolved_type::category::either:
            return std::format(
                "event field '{}' uses 'either<A, B>', which is not "
                "representable in an event payload; {}",
                field_name, allowed);
        case resolved_type::category::holder:
            return std::format(
                "event field '{}' uses 'holder<A>', which is not "
                "representable in an event payload; {}",
                field_name, allowed);
        }
        return std::nullopt;
    }

    /// `#[batch]` requires exactly one `slice<scalar>` field; `#[cancellable]`
    /// is metadata only. Presence/argument-shape checks live in
    /// validate_attributes(); this validates the field shape.
    void validate_event_attributes(decl_model &decl) {
        decl.event_batch = find_attribute(decl.attributes, "batch") != nullptr;
        decl.event_cancellable =
            find_attribute(decl.attributes, "cancellable") != nullptr;
        const auto *batch{find_attribute(decl.attributes, "batch")};
        if (batch == nullptr) {
            return;
        }
        if (decl.fields.size() != 1) {
            error(batch->location,
                  std::format("'#[batch]' event '{}' must declare exactly one "
                              "field (the batch payload), found {}",
                              decl.name, decl.fields.size()));
            return;
        }
        if (decl.fields.front().type.kind != resolved_type::category::slice) {
            error(batch->location,
                  std::format("'#[batch]' event '{}' must declare a "
                              "'slice<scalar>' payload field, but '{}' is not "
                              "a slice",
                              decl.name, decl.fields.front().name));
        }
    }

    [[nodiscard]] auto decl_index_of(std::uint32_t module_index,
                                     const decl_model &decl) const
        -> std::uint32_t {
        const auto &decls{model_.modules[module_index].declarations};
        for (std::uint32_t index = 0; index < decls.size(); ++index) {
            if (&decls[index] == &decl) {
                return index;
            }
        }
        return 0;
    }

    /// Protocol schemas are organised as
    /// `refrain::protocol::v<version>::<stage>::<flow>`: the module name is
    /// the only place the stage and direction are declared, so the generated
    /// packet table can be derived without repeating metadata on every packet.
    void validate_packet_module(std::uint32_t module_index,
                                const decl_model &decl) {
        static constexpr std::array stages{"handshake", "status", "login",
                                           "configuration", "play"};
        static constexpr std::array flows{"serverbound", "clientbound"};
        std::vector<std::string_view> parts;
        const std::string_view name{module_at(module_index).name};
        std::size_t begin{0};
        while (begin <= name.size()) {
            const auto end{name.find("::", begin)};
            if (end == std::string_view::npos) {
                parts.emplace_back(name.substr(begin));
                break;
            }
            parts.emplace_back(name.substr(begin, end - begin));
            begin = end + 2;
        }
        const bool valid{parts.size() == 5 && parts[0] == "refrain" &&
                         parts[1] == "protocol" && parts[2].starts_with("v") &&
                         all_digits(parts[2].substr(1)) &&
                         std::ranges::any_of(stages,
                                             [&](std::string_view stage) {
                                                 return stage == parts[3];
                                             }) &&
                         std::ranges::any_of(flows, [&](std::string_view flow) {
                             return flow == parts[4];
                         })};
        if (!valid) {
            error(
                decl.location,
                std::format("packet '{}' must live in module "
                            "'refrain::protocol::v<version>::<stage>::<flow>'",
                            decl.name));
        }
    }

    void validate_packet(std::uint32_t module_index, decl_model &decl) {
        auto &ids{packet_ids_[module_index]};
        if (!decl.packet_id.has_value()) {
            error(decl.location,
                  std::format("packet '{}' has no wire id", decl.name));
            return;
        }
        const auto id{*decl.packet_id};
        if (id < 0 || id > 0xFFFF) {
            error(decl.location,
                  std::format("packet '{}' wire id 0x{:X} is outside the u16 "
                              "range",
                              decl.name, static_cast<std::uint64_t>(id)));
            return;
        }
        if (ids.contains(id)) {
            error(decl.location,
                  std::format("packet '{}' reuses wire id 0x{:X}", decl.name,
                              static_cast<std::uint64_t>(id)));
            return;
        }
        ids.insert(id);
    }

    [[nodiscard]] auto resolve_field(const field_decl &source,
                                     std::uint32_t current_module)
        -> std::optional<field_model> {
        field_model field;
        field.name = source.name;
        field.documentation = source.documentation;
        field.attributes = source.attributes;
        field.out = source.out;
        field.default_value = source.default_value;
        field.location = source.location;
        auto type{resolve_type(source.type, current_module, false)};
        if (!type.has_value()) {
            return std::nullopt;
        }
        field.type = std::move(*type);
        validate_attributes(field.attributes, source.location,
                            attribute_scope::field);
        apply_wire_attributes(field, module_at(current_module).kind);
        return field;
    }

    /// Resolve `#[varint]`/`#[varlong]`/`#[fixed]`/`#[max]` into the wire
    /// hints consumed by the RSC emitter. These attributes only describe bytes
    /// on the wire, so they are rejected outside RSC schemas and on field types
    /// the selected encoding cannot represent.
    void apply_wire_attributes(field_model &field, dialect kind) {
        const auto *varint{find_attribute(field.attributes, "varint")};
        const auto *varlong{find_attribute(field.attributes, "varlong")};
        const auto *fixed{find_attribute(field.attributes, "fixed")};
        const auto selected{static_cast<int>(varint != nullptr) +
                            static_cast<int>(varlong != nullptr) +
                            static_cast<int>(fixed != nullptr)};
        const auto *max{find_attribute(field.attributes, "max")};
        const auto *fixed_bytes{
            find_attribute(field.attributes, "fixed_bytes")};
        const auto *optional_varint{
            find_attribute(field.attributes, "optional_varint")};
        const auto *rest{find_attribute(field.attributes, "rest")};
        const auto *fixed_count{
            find_attribute(field.attributes, "fixed_count")};
        if (selected == 0 && max == nullptr && fixed_bytes == nullptr &&
            optional_varint == nullptr && rest == nullptr &&
            fixed_count == nullptr) {
            return;
        }
        if (kind != dialect::rsc) {
            error(field.location,
                  "wire encoding attributes are only valid in RSC schemas");
            return;
        }
        if (selected > 1) {
            error(field.location,
                  "'#[varint]', '#[varlong]' and '#[fixed]' are mutually "
                  "exclusive");
            return;
        }
        if (selected == 1) {
            integer_wire wire{integer_wire::natural};
            if (varint != nullptr) {
                wire = integer_wire::varint;
            } else if (varlong != nullptr) {
                wire = integer_wire::varlong;
            } else {
                wire = integer_wire::fixed;
            }
            const auto element{
                field.type.kind == resolved_type::category::array ||
                        field.type.kind == resolved_type::category::slice ||
                        field.type.kind == resolved_type::category::repeated
                    ? &field.type.arguments.front()
                    : &field.type};
            if (element->kind != resolved_type::category::primitive ||
                !is_integer_primitive(element->primitive)) {
                error(field.location,
                      std::format("'#[{}]' requires an integer field",
                                  varint != nullptr    ? "varint"
                                  : varlong != nullptr ? "varlong"
                                                       : "fixed"));
                return;
            }
            if (wire == integer_wire::varlong &&
                element->primitive != primitive_kind::u64 &&
                element->primitive != primitive_kind::i64) {
                error(field.location,
                      "'#[varlong]' requires a 64-bit integer field");
                return;
            }
            field.wire = wire;
        }
        if (fixed_count != nullptr) {
            const auto is_array = [](const resolved_type &type) {
                return type.kind == resolved_type::category::array;
            };
            if (!is_array(field.type)) {
                error(field.location,
                      "'#[fixed_count]' is only valid on an 'array<T>' field");
            } else if (fixed_count->arguments.size() != 1 ||
                       fixed_count->arguments.front().kind !=
                           attribute_argument_kind::integer) {
                error(field.location,
                      "'#[fixed_count]' expects exactly one integer argument");
            } else {
                std::uint64_t count{0};
                const auto &text{fixed_count->arguments.front().text};
                const auto parsed{std::from_chars(
                    text.data(), text.data() + text.size(), count)};
                if (parsed.ec != std::errc{} || count == 0) {
                    error(field.location,
                          "'#[fixed_count]' expects a positive integer");
                } else {
                    field.wire_fixed_count = count;
                }
            }
            return;
        }
        if (rest != nullptr) {
            const auto is_bytes = [](const resolved_type &type) {
                return type.kind == resolved_type::category::primitive &&
                       type.primitive == primitive_kind::bytes;
            };
            if (!is_bytes(field.type)) {
                error(field.location,
                      "'#[rest]' is only valid on a 'bytes' field");
            } else if (max != nullptr || fixed_bytes != nullptr ||
                       selected != 0 || optional_varint != nullptr) {
                error(field.location,
                      "'#[rest]' cannot be combined with a length or integer "
                      "encoding");
            } else {
                field.wire_rest = true;
            }
            return;
        }
        if (fixed_bytes != nullptr) {
            const auto is_bytes = [](const resolved_type &type) {
                return type.kind == resolved_type::category::primitive &&
                       type.primitive == primitive_kind::bytes;
            };
            const auto bytes_field =
                is_bytes(field.type) ||
                (field.type.kind == resolved_type::category::optional &&
                 is_bytes(field.type.arguments.front()));
            if (!bytes_field) {
                error(field.location,
                      "'#[fixed_bytes]' is only valid on a 'bytes' field or "
                      "'optional<bytes>'");
            } else if (fixed_bytes->arguments.size() == 1 &&
                       fixed_bytes->arguments.front().kind ==
                           attribute_argument_kind::integer) {
                std::uint64_t length{0};
                const auto &text{fixed_bytes->arguments.front().text};
                const auto parsed{std::from_chars(
                    text.data(), text.data() + text.size(), length)};
                if (parsed.ec != std::errc{} || length == 0) {
                    error(field.location,
                          "'#[fixed_bytes]' expects a positive integer");
                } else {
                    field.wire_fixed_bytes = length;
                }
            }
        }
        if (optional_varint != nullptr) {
            const auto element{field.type.kind ==
                                       resolved_type::category::optional
                                   ? &field.type.arguments.front()
                                   : nullptr};
            if (element == nullptr ||
                element->kind != resolved_type::category::primitive ||
                !is_integer_primitive(element->primitive)) {
                error(field.location, "'#[optional_varint]' is only valid on "
                                      "'optional<integer>' fields");
            } else {
                field.wire_optional_varint = true;
            }
        }
        if (max != nullptr && max->arguments.size() == 1 &&
            max->arguments.front().kind == attribute_argument_kind::integer) {
            std::uint64_t limit{0};
            const auto &text{max->arguments.front().text};
            const auto parsed{
                std::from_chars(text.data(), text.data() + text.size(), limit)};
            if (parsed.ec == std::errc{} && limit > 0) {
                const auto bounded{
                    field.type.kind == resolved_type::category::primitive
                        ? field.type.primitive == primitive_kind::string ||
                              field.type.primitive == primitive_kind::bytes
                        : field.type.kind == resolved_type::category::array ||
                              field.type.kind ==
                                  resolved_type::category::slice ||
                              field.type.kind ==
                                  resolved_type::category::repeated};
                if (!bounded) {
                    error(field.location,
                          "'#[max]' is only valid on strings, bytes and "
                          "arrays");
                } else {
                    field.wire_max = limit;
                }
            }
        }
    }

    [[nodiscard]] auto resolve_type(const type_ref &source,
                                    std::uint32_t current_module,
                                    bool allow_void)
        -> std::optional<resolved_type> {
        // The parser already bounds nesting, but keep the semantic
        // resolver bounded too (defense in depth for programmatic callers).
        if (resolve_depth_ >= max_type_depth) {
            error(source.location,
                  std::format("type nesting exceeds the maximum depth of {}",
                              max_type_depth));
            return std::nullopt;
        }
        ++resolve_depth_;
        struct depth_guard {
            std::uint32_t &depth;
            ~depth_guard() { --depth; }
        };
        const depth_guard guard{resolve_depth_};

        const auto current_kind{module_at(current_module).kind};
        const auto named_allowed =
            [&](const decl_model &decl) -> std::optional<std::string> {
            if (module_kind_of_decl(&decl) != current_kind) {
                return "RSC data types and RIDL contract types cannot be "
                       "mixed in one schema graph";
            }
            switch (decl.kind) {
            case declaration_kind::structure:
            case declaration_kind::enumeration:
            case declaration_kind::flags:
                return std::nullopt;
            case declaration_kind::record:
                return current_kind == dialect::rsc
                           ? std::nullopt
                           : std::optional<std::string>{
                                 "records cannot be referenced from RIDL"};
            case declaration_kind::packet:
                return std::optional<std::string>{
                    "packets cannot be referenced by name as field types"};
            case declaration_kind::handle:
            case declaration_kind::service:
                return std::optional<std::string>{
                    "this declaration cannot be used as a value type"};
            case declaration_kind::event:
                return std::optional<std::string>{
                    "events cannot be referenced by name as field types"};
            }
            return std::nullopt;
        };

        if (source.name == "handle") {
            if (source.arguments.size() != 1 ||
                !source.arguments.front().arguments.empty()) {
                error(source.location,
                      "'handle<T>' expects exactly one type argument");
                return std::nullopt;
            }
            const auto tag{source.arguments.front().name};
            const auto handle{find_handle(tag)};
            if (!handle.has_value()) {
                error(source.location,
                      std::format("unknown handle tag '{}' (declare it with "
                                  "`handle<{}>;`)",
                                  tag, tag));
                return std::nullopt;
            }
            return make_handle_type(*handle);
        }

        if (source.arguments.empty()) {
            if (const auto primitive{primitive_from_name(source.name)};
                primitive.has_value()) {
                if (*primitive == primitive_kind::void_type && !allow_void) {
                    error(source.location,
                          "'void' is only valid as a method result type");
                    return std::nullopt;
                }
                if (current_kind == dialect::ridl &&
                    (*primitive == primitive_kind::identifier ||
                     *primitive == primitive_kind::uuid ||
                     *primitive == primitive_kind::text_component ||
                     *primitive == primitive_kind::item_stack ||
                     *primitive == primitive_kind::lp_vec3 ||
                     *primitive == primitive_kind::nbt ||
                     *primitive == primitive_kind::varint_id ||
                     *primitive == primitive_kind::message_signature_packed)) {
                    error(source.location,
                          std::format("'{}' is only valid in RSC wire "
                                      "schemas",
                                      primitive_name(*primitive)));
                    return std::nullopt;
                }
                resolved_type type;
                type.kind = resolved_type::category::primitive;
                type.primitive = *primitive;
                return type;
            }
            if (const auto handle{find_handle(source.name)};
                handle.has_value()) {
                return make_handle_type(*handle);
            }
            const auto diagnostics_before{sink_.size()};
            const auto symbol{
                find_named(source.name, current_module, source.location)};
            if (!symbol.has_value()) {
                if (sink_.size() == diagnostics_before) {
                    error(source.location,
                          std::format("unknown type '{}' (declare it in this "
                                      "module or 'import' its module)",
                                      source.name));
                }
                return std::nullopt;
            }
            if (const auto rejection{named_allowed(decl_at(*symbol))};
                rejection.has_value()) {
                error(source.location, *rejection);
                return std::nullopt;
            }
            resolved_type type;
            type.kind = resolved_type::category::named;
            type.module_index = symbol->module;
            type.decl_index = symbol->decl;
            return type;
        }

        const auto &generic{source.name};
        if (generic == "holder") {
            if (source.arguments.size() != 1) {
                error(source.location,
                      "'holder<A>' expects exactly one type argument");
                return std::nullopt;
            }
            auto direct{
                resolve_type(source.arguments[0], current_module, false)};
            if (!direct.has_value()) {
                return std::nullopt;
            }
            resolved_type type;
            type.kind = resolved_type::category::holder;
            type.arguments.push_back(std::move(*direct));
            return type;
        }
        if (generic == "either") {
            if (source.arguments.size() != 2) {
                error(source.location,
                      "'either<A, B>' expects exactly two type arguments");
                return std::nullopt;
            }
            auto left{resolve_type(source.arguments[0], current_module, false)};
            auto right{
                resolve_type(source.arguments[1], current_module, false)};
            if (!left.has_value() || !right.has_value()) {
                return std::nullopt;
            }
            resolved_type type;
            type.kind = resolved_type::category::either;
            type.arguments.push_back(std::move(*left));
            type.arguments.push_back(std::move(*right));
            return type;
        }
        if (generic == "map") {
            if (source.arguments.size() != 2) {
                error(source.location,
                      "'map<K, V>' expects exactly two type arguments");
                return std::nullopt;
            }
            auto key{resolve_type(source.arguments[0], current_module, false)};
            auto value{
                resolve_type(source.arguments[1], current_module, false)};
            if (!key.has_value() || !value.has_value()) {
                return std::nullopt;
            }
            if (key->kind == resolved_type::category::array ||
                key->kind == resolved_type::category::map ||
                key->kind == resolved_type::category::optional ||
                key->kind == resolved_type::category::slice ||
                key->kind == resolved_type::category::repeated) {
                error(source.location,
                      "'map<K, V>' requires a scalar or named key type");
                return std::nullopt;
            }
            resolved_type type;
            type.kind = resolved_type::category::map;
            type.arguments.push_back(std::move(*key));
            type.arguments.push_back(std::move(*value));
            return type;
        }
        if (generic == "array" || generic == "slice" || generic == "optional" ||
            generic == "repeated") {
            if (source.arguments.size() != 1) {
                error(source.location,
                      std::format("'{}<T>' expects exactly one type argument",
                                  generic));
                return std::nullopt;
            }
            auto element{
                resolve_type(source.arguments.front(), current_module, false)};
            if (!element.has_value()) {
                return std::nullopt;
            }
            resolved_type type;
            if (generic == "array") {
                type.kind = resolved_type::category::array;
            } else if (generic == "slice") {
                type.kind = resolved_type::category::slice;
            } else if (generic == "optional") {
                type.kind = resolved_type::category::optional;
            } else {
                type.kind = resolved_type::category::repeated;
            }
            type.arguments.push_back(std::move(*element));
            return type;
        }
        if (generic == "result") {
            error(source.location,
                  "'result<T, E>' is only valid as a method return type");
            return std::nullopt;
        }
        error(source.location,
              std::format("unknown generic type '{}'", generic));
        return std::nullopt;
    }

    [[nodiscard]] auto module_kind_of_decl(const decl_model *decl) const
        -> dialect {
        for (const auto &module : model_.modules) {
            for (const auto &candidate : module.declarations) {
                if (&candidate == decl) {
                    return module.kind;
                }
            }
        }
        return dialect::rsc;
    }

    [[nodiscard]] static auto make_handle_type(decl_ref ref) -> resolved_type {
        resolved_type type;
        type.kind = resolved_type::category::handle;
        type.module_index = ref.module;
        type.decl_index = ref.decl;
        return type;
    }

    [[nodiscard]] auto
    is_valid_contract_field_type(const resolved_type &type) const -> bool {
        switch (type.kind) {
        case resolved_type::category::primitive:
            return type.primitive != primitive_kind::void_type;
        case resolved_type::category::named:
            return true;
        case resolved_type::category::handle:
            return true;
        case resolved_type::category::slice: {
            if (type.arguments.size() != 1) {
                return false;
            }
            const auto &element{type.arguments.front()};
            if (element.kind != resolved_type::category::named) {
                return false;
            }
            return decl_at({element.module_index, element.decl_index}).kind ==
                   declaration_kind::structure;
        }
        case resolved_type::category::array:
        case resolved_type::category::optional:
        case resolved_type::category::repeated:
        case resolved_type::category::map:
        case resolved_type::category::either:
        case resolved_type::category::holder:
            return false;
        }
        return false;
    }

    [[nodiscard]] auto is_valid_data_field_type(const resolved_type &type) const
        -> bool {
        switch (type.kind) {
        case resolved_type::category::primitive:
            return type.primitive != primitive_kind::void_type;
        case resolved_type::category::named:
            return true;
        case resolved_type::category::handle:
            return false;
        case resolved_type::category::array:
        case resolved_type::category::slice:
        case resolved_type::category::optional:
        case resolved_type::category::repeated:
            return type.arguments.size() == 1 &&
                   is_valid_data_field_type(type.arguments.front());
        case resolved_type::category::map:
        case resolved_type::category::either:
            return type.arguments.size() == 2 &&
                   is_valid_data_field_type(type.arguments[0]) &&
                   is_valid_data_field_type(type.arguments[1]);
        case resolved_type::category::holder:
            return type.arguments.size() == 1 &&
                   is_valid_data_field_type(type.arguments[0]);
        }
        return false;
    }

    void resolve_enum(decl_model &decl) {
        const auto info{integer_info_of(decl.underlying)};
        if (!info.has_value()) {
            error(decl.location,
                  std::format("{} '{}' must have an integer underlying type, "
                              "got '{}'",
                              declaration_kind_name(decl.kind), decl.name,
                              decl.underlying));
            return;
        }
        std::set<std::string> names;
        std::set<std::int64_t> values;
        // `next_value = value + 1` is signed-overflow UB for an explicit
        // INT64_MAX entry; track it as an optional and report instead.
        std::optional<std::int64_t> next_value{0};
        std::size_t entry_index{0};
        for (auto &entry : decl.entries) {
            if (!names.insert(entry.name).second) {
                error(entry.location,
                      std::format("duplicate {} entry '{}'",
                                  declaration_kind_name(decl.kind),
                                  entry.name));
            }
            std::int64_t value{0};
            if (entry.value.has_value()) {
                value = *entry.value;
            } else if (decl.kind == declaration_kind::flags) {
                if (entry_index >= 63) {
                    error(entry.location,
                          "auto-numbered flags support at most 63 entries");
                    // Do not shift by 63/64 (UB) and do not continue with a
                    // bogus value: that would report a second, misleading
                    // "does not fit" error for the same entry.
                    ++entry_index;
                    continue;
                }
                value = static_cast<std::int64_t>(1) << entry_index;
            } else if (next_value.has_value()) {
                value = *next_value;
            } else {
                error(entry.location,
                      std::format("cannot auto-number {} entry '{}': the "
                                  "previous value is the maximum int64; give "
                                  "this entry an explicit value",
                                  declaration_kind_name(decl.kind),
                                  entry.name));
                value = 0;
            }
            ++entry_index;
            if (!fits_integer(value, *info)) {
                error(entry.location, std::format("value {} does not fit '{}'",
                                                  value, decl.underlying));
            }
            if (!values.insert(value).second) {
                error(entry.location,
                      std::format("duplicate {} value {}",
                                  declaration_kind_name(decl.kind), value));
            }
            entry.value = value;
            if (value == std::numeric_limits<std::int64_t>::max()) {
                next_value.reset();
            } else {
                next_value = value + 1;
            }
        }
    }

    void resolve_service(std::uint32_t module_index, decl_model &decl,
                         const declaration &source) {
        static_cast<void>(module_index);
        const auto version{
            parse_service_version(decl.service_version, decl.location)};
        if (!version.has_value()) {
            return;
        }
        decl.service_version_major = version->major;
        decl.service_version_minor = version->minor;
        decl.service_version_patch = version->patch;
        for (const auto &provided : decl.service.provides) {
            if (provided.empty()) {
                error(decl.location,
                      std::format("service '{}' has an empty 'provides' entry",
                                  decl.name));
            }
        }
        for (const auto &required : decl.service.requires_list) {
            validate_requires(required, decl);
        }
        static constexpr std::array sides{"server", "client", "both"};
        static constexpr std::array threads{"main",   "sim", "render", "any",
                                            "worker", "io",  "net"};
        static constexpr std::array compositions{"exclusive", "instanced",
                                                 "chain", "registry"};
        if (std::ranges::find(sides, decl.service.side) == sides.end()) {
            error(decl.location,
                  std::format("invalid service side '{}'", decl.service.side));
        }
        if (std::ranges::find(threads, decl.service.thread) == threads.end()) {
            error(decl.location, std::format("invalid service thread '{}'",
                                             decl.service.thread));
        }
        if (std::ranges::find(compositions, decl.service.composition) ==
            compositions.end()) {
            error(decl.location,
                  std::format("invalid service composition '{}' (expected "
                              "'exclusive', 'instanced', 'chain' or "
                              "'registry')",
                              decl.service.composition));
        }
        if (decl.service.provides.empty()) {
            error(decl.location,
                  std::format("service '{}' must declare at least one "
                              "'provides' entry",
                              decl.name));
        }
        resolve_methods(module_index, decl, source);
        if (decl.methods.empty()) {
            error(decl.location,
                  std::format("service '{}' has no methods", decl.name));
        }
        std::set<std::string> method_names;
        for (const auto &method : decl.methods) {
            if (!method_names.insert(method.name).second) {
                error(method.location,
                      std::format("duplicate method '{}' in service '{}'",
                                  method.name, decl.name));
            }
        }
    }

    void resolve_methods(std::uint32_t module_index, decl_model &decl,
                         const declaration &source) {
        for (const auto &method_source : source.methods) {
            validate_attributes(method_source.attributes,
                                method_source.location,
                                attribute_scope::method);
            method_model method;
            method.name = method_source.name;
            method.documentation = method_source.documentation;
            method.attributes = method_source.attributes;
            method.location = method_source.location;

            std::set<std::string> parameter_names;
            for (const auto &parameter_source : method_source.parameters) {
                auto parameter{resolve_field(parameter_source, module_index)};
                if (!parameter.has_value()) {
                    continue;
                }
                if (!parameter_names.insert(parameter->name).second) {
                    error(parameter->location,
                          std::format("duplicate parameter '{}'",
                                      parameter->name));
                }
                if (!is_valid_contract_field_type(parameter->type)) {
                    error(parameter->location,
                          std::format("parameter '{}' has a type that is not "
                                      "allowed in a service method",
                                      parameter->name));
                    continue;
                }
                if (parameter->out) {
                    if (parameter->type.kind !=
                            resolved_type::category::named ||
                        decl_at({parameter->type.module_index,
                                 parameter->type.decl_index})
                                .kind != declaration_kind::structure) {
                        error(parameter->location,
                              std::format("'out' parameter '{}' must be a "
                                          "contract struct",
                                          parameter->name));
                        continue;
                    }
                }
                method.parameters.push_back(std::move(*parameter));
            }

            method.result_type = resolved_type{};
            method.result_type.kind = resolved_type::category::primitive;
            method.result_type.primitive = primitive_kind::void_type;

            if (method_source.has_return) {
                if (method_source.return_type.name == "result") {
                    if (method_source.return_type.arguments.size() != 2) {
                        error(method_source.location,
                              "'result<T, E>' expects exactly two type "
                              "arguments");
                        continue;
                    }
                    auto success{
                        resolve_type(method_source.return_type.arguments[0],
                                     module_index, true)};
                    auto failure{
                        resolve_type(method_source.return_type.arguments[1],
                                     module_index, false)};
                    if (!success.has_value() || !failure.has_value()) {
                        continue;
                    }
                    if (!is_error_enum(*failure)) {
                        error(method_source.location,
                              "'result<T, E>' requires E to be an enum with a "
                              "u32 underlying type and no zero value");
                        continue;
                    }
                    // Container types (array/optional/repeated) have no C
                    // ABI representation; emitting them as `void` (a
                    // type-erased pointer) produces C++ headers that do not
                    // compile.
                    if (!success->is_void() &&
                        !is_valid_contract_field_type(*success)) {
                        error(method_source.location,
                              std::format("'result<T, E>' success type is not "
                                          "representable in the C ABI v0 "
                                          "(array/optional/repeated are RSC "
                                          "container types; use "
                                          "slice<struct> or an 'out' "
                                          "parameter)"));
                        continue;
                    }
                    method.can_fail = true;
                    method.result_type = std::move(*success);
                    method.error_type = std::move(*failure);
                } else {
                    auto result{resolve_type(method_source.return_type,
                                             module_index, true)};
                    if (!result.has_value()) {
                        continue;
                    }
                    if (result->kind == resolved_type::category::named &&
                        decl_at({result->module_index, result->decl_index})
                                .kind == declaration_kind::structure) {
                        error(method_source.location,
                              "struct return values must use 'result<T, E>' "
                              "or an explicit 'out' parameter");
                        continue;
                    }
                    if (!result->is_void() &&
                        !is_valid_contract_field_type(*result)) {
                        error(method_source.location,
                              "method return type is not representable in the "
                              "C ABI v0 (array/optional/repeated are RSC "
                              "container types)");
                        continue;
                    }
                    method.result_type = std::move(*result);
                }
            }
            if (const auto *schedule{
                    find_attribute(method_source.attributes, "schedule")};
                schedule != nullptr) {
                resolve_schedule(*schedule, method, method_source);
            }
            decl.methods.push_back(std::move(method));
        }
    }

    /// Validate `#[schedule(...)]` and record the parsed descriptor metadata.
    /// Accepted form: named arguments only, `interval` required (decimal
    /// 1..1'000'000), `phase` optional (`pre_tick`|`post_tick`, default
    /// `post_tick`); the method takes no parameters and returns `void` or
    /// `result<void, E>`.
    void resolve_schedule(const attribute &schedule, method_model &method,
                          const method_decl &source) {
        auto valid{true};
        std::optional<std::int64_t> interval;
        auto phase_of{schedule_phase_kind::post_tick};
        bool interval_seen{false};
        bool phase_seen{false};
        if (schedule.arguments.empty()) {
            error(schedule.location,
                  "'#[schedule]' requires the named argument 'interval'");
            valid = false;
        }
        for (const auto &argument : schedule.arguments) {
            if (!argument.name.has_value()) {
                error(schedule.location,
                      std::format("'#[schedule]' arguments must be named "
                                  "('interval=N', 'phase=pre_tick|post_tick'); "
                                  "positional argument '{}' is not accepted",
                                  argument.text));
                valid = false;
                continue;
            }
            if (*argument.name == "interval") {
                if (interval_seen) {
                    error(schedule.location,
                          "'#[schedule]' repeats the named argument "
                          "'interval'");
                    valid = false;
                    continue;
                }
                interval_seen = true;
                interval = parse_schedule_interval(argument, schedule.location);
                if (!interval.has_value()) {
                    valid = false;
                }
            } else if (*argument.name == "phase") {
                if (phase_seen) {
                    error(schedule.location,
                          "'#[schedule]' repeats the named argument 'phase'");
                    valid = false;
                    continue;
                }
                phase_seen = true;
                if (argument.kind != attribute_argument_kind::identifier ||
                    (argument.text != "pre_tick" &&
                     argument.text != "post_tick")) {
                    error(schedule.location,
                          std::format("invalid '#[schedule]' phase '{}' "
                                      "(expected 'pre_tick' or 'post_tick')",
                                      argument.text));
                    valid = false;
                    continue;
                }
                phase_of = argument.text == "pre_tick"
                               ? schedule_phase_kind::pre_tick
                               : schedule_phase_kind::post_tick;
            } else {
                error(schedule.location,
                      std::format("unknown '#[schedule]' named argument '{}' "
                                  "(expected 'interval' or 'phase')",
                                  *argument.name));
                valid = false;
            }
        }
        if (!interval.has_value() && valid) {
            error(schedule.location,
                  "'#[schedule]' requires the named argument 'interval'");
            valid = false;
        }
        if (!source.parameters.empty()) {
            error(source.location,
                  std::format("'#[schedule]' method '{}' must take no "
                              "parameters; a periodic task receives no "
                              "arguments",
                              method.name));
            valid = false;
        }
        if (!method.result_type.is_void()) {
            error(source.location,
                  std::format("'#[schedule]' method '{}' must return 'void' "
                              "or 'result<void, E>'",
                              method.name));
            valid = false;
        }
        if (!valid || !interval.has_value()) {
            return;
        }
        method.scheduled = true;
        method.schedule_interval_ticks = *interval;
        method.schedule_phase_of = phase_of;
    }

    /// `#[schedule]` interval is a decimal integer token in the shared
    /// registration range (schedule.hpp constants, so a schema-legal value is
    /// never rejected by `scheduler::add`).
    [[nodiscard]] auto
    parse_schedule_interval(const attribute_argument &argument,
                            const source_location &where)
        -> std::optional<std::int64_t> {
        if (argument.kind != attribute_argument_kind::integer ||
            !all_digits(argument.text)) {
            error(where,
                  std::format("'#[schedule]' interval '{}' must be a "
                              "decimal integer between {} and {}",
                              argument.text, core::schedule::min_interval_ticks,
                              core::schedule::max_interval_ticks));
            return std::nullopt;
        }
        std::int64_t value{0};
        const auto *begin{argument.text.data()};
        const auto *end{begin + argument.text.size()};
        const auto [parse_end, parse_error]{std::from_chars(begin, end, value)};
        if (parse_error != std::errc{} || parse_end != end) {
            error(where,
                  std::format("'#[schedule]' interval '{}' is out of "
                              "range (expected {}..{})",
                              argument.text, core::schedule::min_interval_ticks,
                              core::schedule::max_interval_ticks));
            return std::nullopt;
        }
        if (value < core::schedule::min_interval_ticks ||
            value > core::schedule::max_interval_ticks) {
            error(where, std::format("'#[schedule]' interval {} is out of "
                                     "range (expected {}..{})",
                                     value, core::schedule::min_interval_ticks,
                                     core::schedule::max_interval_ticks));
            return std::nullopt;
        }
        return value;
    }

    [[nodiscard]] auto is_error_enum(const resolved_type &type) const -> bool {
        if (type.kind != resolved_type::category::named) {
            return false;
        }
        const auto &decl{decl_at({type.module_index, type.decl_index})};
        if (decl.kind != declaration_kind::enumeration) {
            return false;
        }
        const auto info{integer_info_of(decl.underlying)};
        if (!info.has_value() || info->bits != 32 || info->is_signed) {
            return false;
        }
        return std::ranges::none_of(decl.entries, [](const enum_entry &entry) {
            return entry.value.value_or(0) == 0;
        });
    }

    struct service_version {
        std::uint32_t major{0};
        std::uint32_t minor{0};
        std::uint32_t patch{0};
    };

    /// Full SemVer 2.0.0 service version. Parsing is delegated to the shared
    /// `refrain::core::semver` (same parser and rules as the gamepack
    /// resolver); sema stores the parsed triple in the model so the emitters
    /// never re-parse the version string. Published contract services
    /// must be release versions: the emitted ABI constants have no place to
    /// encode prerelease identifiers, while build metadata is accepted and
    /// ignored for ABI purposes.
    [[nodiscard]] auto parse_service_version(std::string_view text,
                                             const source_location &where)
        -> std::optional<service_version> {
        const auto parsed{core::semver::parse(text)};
        if (!parsed.has_value()) {
            error(where,
                  std::format("invalid service version '{}': expected a full "
                              "SemVer 2.0.0 version major.minor.patch ({})",
                              text, error_text(parsed.error())));
            return std::nullopt;
        }
        if (parsed->is_prerelease()) {
            error(where,
                  std::format("invalid service version '{}': published "
                              "contracts must declare a release version "
                              "(prerelease identifiers cannot be encoded in "
                              "the ABI constants; use build metadata instead, "
                              "e.g. 1.0.0+dev)",
                              text));
            return std::nullopt;
        }
        return service_version{parsed->major, parsed->minor, parsed->patch};
    }

    /// Validate one RIDL `requires` entry: `<service_id><op><semver>` with
    /// op ∈ {>=, ^, ~, =}. The entry is kept verbatim (the emitters write it to
    /// `_REQUIRES`); this only rejects malformed strings with a readable
    /// diagnostic so a typo cannot silently produce an unenforceable range.
    void validate_requires(std::string_view entry, const decl_model &decl) {
        static constexpr std::array<std::string_view, 4> operators{">=", "^",
                                                                   "~", "="};
        std::size_t operator_position{std::string_view::npos};
        std::string_view matched_operator;
        for (const auto candidate : operators) {
            const auto position{entry.find(candidate)};
            if (position != std::string_view::npos &&
                (operator_position == std::string_view::npos ||
                 position < operator_position)) {
                operator_position = position;
                matched_operator = candidate;
            }
        }
        if (operator_position == std::string_view::npos) {
            error(decl.location,
                  std::format("invalid 'requires' entry '{}': expected "
                              "'<service_id><op><semver>' with op one of "
                              "'>=', '^', '~', '='",
                              entry));
            return;
        }
        const auto service_id{entry.substr(0, operator_position)};
        const auto version_text{
            entry.substr(operator_position + matched_operator.size())};
        const auto valid_id_character = [](char character) {
            return character != ' ' && character != '\t' && character != '<' &&
                   character != '>';
        };
        if (service_id.empty() ||
            !std::ranges::all_of(service_id, valid_id_character)) {
            error(decl.location,
                  std::format("invalid 'requires' entry '{}': the service id "
                              "'{}' must be a non-empty id without "
                              "whitespace or comparison characters",
                              entry, service_id));
            return;
        }
        const auto parsed{core::semver::parse(version_text)};
        if (!parsed.has_value()) {
            error(decl.location,
                  std::format("invalid 'requires' entry '{}': the version "
                              "'{}' is not a full SemVer 2.0.0 version ({})",
                              entry, version_text, error_text(parsed.error())));
        }
    }

    void validate_attributes(
        const std::vector<attribute> &attributes, const source_location &where,
        attribute_scope scope,
        declaration_kind declaration_type = declaration_kind::structure) {
        static_cast<void>(where);
        const auto on_event{scope == attribute_scope::declaration &&
                            declaration_type == declaration_kind::event};
        std::set<std::string> seen;
        for (const auto &attribute : attributes) {
            if (!seen.insert(attribute.name).second) {
                error(
                    attribute.location,
                    std::format("duplicate attribute '#[{}]'", attribute.name));
            }
            if (!is_known_attribute(attribute.name)) {
                error(attribute.location,
                      std::format("unknown attribute '#[{}]'", attribute.name));
                continue;
            }
            // RIDL event attributes: `#[batch]`/`#[cancellable]` are
            // event-declaration only and take no arguments. `#[schedule]` is
            // method-only; its arguments are validated with the method
            // signature in resolve_schedule().
            if (attribute.name == "batch" || attribute.name == "cancellable") {
                if (!on_event) {
                    error(attribute.location,
                          std::format("'#[{}]' is only valid on event "
                                      "declarations",
                                      attribute.name));
                } else if (!attribute.arguments.empty()) {
                    error(attribute.location,
                          std::format("'#[{}]' takes no arguments",
                                      attribute.name));
                }
                continue;
            }
            if (attribute.name == "schedule") {
                if (scope != attribute_scope::method) {
                    error(attribute.location,
                          "'#[schedule]' is only valid on service methods");
                }
                continue;
            }
            if ((attribute.name == "thread" || attribute.name == "side") &&
                attribute.arguments.size() != 1) {
                error(attribute.location,
                      std::format("'#[{}]' expects exactly one argument",
                                  attribute.name));
                continue;
            }
            if (attribute.name == "varint" || attribute.name == "varlong" ||
                attribute.name == "fixed" ||
                attribute.name == "optional_varint" ||
                attribute.name == "rest" || attribute.name == "zigzag") {
                if (!attribute.arguments.empty()) {
                    error(attribute.location,
                          std::format("'#[{}]' takes no arguments",
                                      attribute.name));
                }
                continue;
            }
            if (attribute.name == "fixed_bytes" || attribute.name == "max" ||
                attribute.name == "fixed_count") {
                const auto name{attribute.name};
                if (attribute.arguments.size() != 1) {
                    error(attribute.location,
                          std::format("'#[{}]' expects exactly one integer "
                                      "argument",
                                      name));
                } else if (attribute.arguments.front().kind !=
                           attribute_argument_kind::integer) {
                    error(attribute.location,
                          std::format("'#[{}]' expects an integer argument",
                                      name));
                } else {
                    std::uint64_t limit{0};
                    const auto &text{attribute.arguments.front().text};
                    const auto *first{text.data()};
                    const auto *last{first + text.size()};
                    const auto parsed{std::from_chars(first, last, limit)};
                    if (parsed.ec != std::errc{} || parsed.ptr != last ||
                        limit == 0) {
                        error(attribute.location,
                              std::format("'#[{}]' expects a positive "
                                          "integer",
                                          name));
                    }
                }
                continue;
            }
            if (attribute.arguments.empty()) {
                if (attribute.name == "since" || attribute.name == "bits" ||
                    attribute.name == "max") {
                    error(attribute.location,
                          std::format("'#[{}]' expects an argument",
                                      attribute.name));
                }
                continue;
            }
            if (attribute.name == "thread") {
                const auto &argument{attribute.arguments.front().text};
                if (argument != "main" && argument != "sim" &&
                    argument != "render" && argument != "any" &&
                    argument != "worker" && argument != "io" &&
                    argument != "net") {
                    error(
                        attribute.location,
                        std::format("invalid thread affinity '{}'", argument));
                }
            }
            if (attribute.name == "side") {
                const auto &argument{attribute.arguments.front().text};
                if (argument != "client" && argument != "server" &&
                    argument != "both") {
                    error(attribute.location,
                          std::format("invalid side '{}'", argument));
                }
            }
            if (attribute.name == "since" &&
                attribute.arguments.front().kind ==
                    attribute_argument_kind::identifier) {
                error(attribute.location,
                      "'#[since]' expects a numeric version");
            }
        }
    }

    // -- phase 4: ABI layout / data cycle checks --------------------------
    void compute_abi() {
        for (std::uint32_t module_index = 0;
             module_index < model_.modules.size(); ++module_index) {
            const auto &module{module_at(module_index)};
            for (std::uint32_t decl_index = 0;
                 decl_index < module.declarations.size(); ++decl_index) {
                const auto &decl{module.declarations[decl_index]};
                if (module.kind == dialect::ridl) {
                    if (decl.kind == declaration_kind::structure) {
                        static_cast<void>(layout_of(module_index, decl_index));
                    } else if (decl.kind == declaration_kind::event) {
                        layout_of_event(module_index, decl_index);
                    }
                } else if (is_struct_like(decl)) {
                    check_data_cycles(module_index, decl_index);
                }
            }
        }
    }

    [[nodiscard]] auto is_struct_like(const decl_model &decl) const noexcept
        -> bool {
        return decl.kind == declaration_kind::structure ||
               decl.kind == declaration_kind::record ||
               decl.kind == declaration_kind::packet;
    }

    [[nodiscard]] auto layout_of(std::uint32_t module_index,
                                 std::uint32_t decl_index)
        -> std::optional<size_align> {
        const auto key{qualified(
            module_index,
            model_.modules[module_index].declarations[decl_index].name)};
        if (const auto found{layouts_.find(key)}; found != layouts_.end()) {
            return found->second;
        }
        if (layout_stack_.contains(key)) {
            error(
                model_.modules[module_index].declarations[decl_index].location,
                std::format("recursive contract struct '{}'", key));
            return std::nullopt;
        }

        layout_stack_.insert(key);
        auto &decl{model_.modules[module_index].declarations[decl_index]};
        std::uint64_t offset{4}; // uint32_t struct_size
        std::uint32_t alignment{4};
        for (auto &field : decl.fields) {
            const auto size{abi_size_of(field.type)};
            if (!size.has_value()) {
                layout_stack_.erase(key);
                return std::nullopt;
            }
            field.abi_alignment = size->alignment;
            field.abi_size = size->size;
            field.abi_offset =
                static_cast<std::uint32_t>(align_up(offset, size->alignment));
            offset = field.abi_offset + size->size;
            alignment = std::max(alignment, size->alignment);
        }
        decl.abi_alignment = alignment;
        decl.abi_size = static_cast<std::uint32_t>(align_up(offset, alignment));
        const size_align result{decl.abi_size, decl.abi_alignment};
        layouts_.emplace(key, result);
        layout_stack_.erase(key);
        return result;
    }

    /// Event layout: the contract-struct padding algorithm without the leading
    /// `uint32_t struct_size` word (events are in-process values, not ABI
    /// structs). Field offsets feed the generated explicit
    /// `reserved_N` padding and the `static_assert` block.
    void layout_of_event(std::uint32_t module_index, std::uint32_t decl_index) {
        auto &decl{model_.modules[module_index].declarations[decl_index]};
        std::uint64_t offset{0};
        std::uint32_t alignment{1};
        for (auto &field : decl.fields) {
            const auto size{abi_size_of(field.type)};
            if (!size.has_value()) {
                return;
            }
            field.abi_alignment = size->alignment;
            field.abi_size = size->size;
            field.abi_offset =
                static_cast<std::uint32_t>(align_up(offset, size->alignment));
            offset = field.abi_offset + size->size;
            alignment = std::max(alignment, size->alignment);
        }
        decl.abi_alignment = alignment;
        decl.abi_size = static_cast<std::uint32_t>(align_up(offset, alignment));
    }

    [[nodiscard]] auto abi_size_of(const resolved_type &type)
        -> std::optional<size_align> {
        switch (type.kind) {
        case resolved_type::category::primitive:
            switch (type.primitive) {
            case primitive_kind::u8:
            case primitive_kind::i8:
            case primitive_kind::boolean:
                return size_align{1, 1};
            case primitive_kind::u16:
            case primitive_kind::i16:
                return size_align{2, 2};
            case primitive_kind::u32:
            case primitive_kind::i32:
            case primitive_kind::f32:
                return size_align{4, 4};
            case primitive_kind::u64:
            case primitive_kind::i64:
            case primitive_kind::f64:
                return size_align{8, 8};
            case primitive_kind::string:
            case primitive_kind::bytes:
                return size_align{16, 8};
            case primitive_kind::identifier:
            case primitive_kind::uuid:
            case primitive_kind::text_component:
            case primitive_kind::item_stack:
            case primitive_kind::lp_vec3:
            case primitive_kind::nbt:
            case primitive_kind::varint_id:
            case primitive_kind::message_signature_packed:
            case primitive_kind::filter_mask:
            case primitive_kind::stat_entry:
            case primitive_kind::position_path:
            case primitive_kind::map_patch:
            case primitive_kind::advancement_seen:
            case primitive_kind::vec_delta:
            case primitive_kind::sound_selector:
            case primitive_kind::objective_update:
            case primitive_kind::team_update:
            case primitive_kind::player_info_update:
            case primitive_kind::tracked_waypoint:
                // Rejected for RIDL in resolve_type(); a wire-only primitive
                // has no in-process ABI layout.
                return std::nullopt;
            case primitive_kind::void_type:
                return std::nullopt;
            }
            break;
        case resolved_type::category::handle:
            return size_align{8, 8};
        case resolved_type::category::slice:
            return size_align{16, 8};
        case resolved_type::category::named: {
            const auto &decl{decl_at({type.module_index, type.decl_index})};
            if (decl.kind == declaration_kind::enumeration ||
                decl.kind == declaration_kind::flags) {
                const auto info{integer_info_of(decl.underlying)};
                if (!info.has_value()) {
                    return std::nullopt;
                }
                const auto bytes{static_cast<std::uint32_t>(info->bits / 8)};
                return size_align{bytes, bytes};
            }
            if (decl.kind == declaration_kind::structure) {
                return layout_of(type.module_index, type.decl_index);
            }
            return std::nullopt;
        }
        case resolved_type::category::array:
        case resolved_type::category::optional:
        case resolved_type::category::repeated:
        case resolved_type::category::map:
        case resolved_type::category::either:
        case resolved_type::category::holder:
            return std::nullopt;
        }
        return std::nullopt;
    }

    void check_data_cycles(std::uint32_t module_index,
                           std::uint32_t decl_index) {
        const auto key{qualified(
            module_index,
            model_.modules[module_index].declarations[decl_index].name)};
        if (data_cycle_done_.contains(key)) {
            return;
        }
        if (data_cycle_stack_.contains(key)) {
            error(
                model_.modules[module_index].declarations[decl_index].location,
                std::format("recursive data type '{}' (split the type or "
                            "use a stable id)",
                            key));
            return;
        }
        data_cycle_stack_.insert(key);
        const auto &decl{model_.modules[module_index].declarations[decl_index]};
        for (const auto &field : decl.fields) {
            visit_data_type(field.type);
        }
        data_cycle_stack_.erase(key);
        data_cycle_done_.insert(key);
    }

    void visit_data_type(const resolved_type &type) {
        if (type.kind == resolved_type::category::named) {
            const auto &decl{decl_at({type.module_index, type.decl_index})};
            if (is_struct_like(decl)) {
                check_data_cycles(type.module_index, type.decl_index);
            }
            return;
        }
        for (const auto &argument : type.arguments) {
            visit_data_type(argument);
        }
    }

    std::vector<source_unit> units_;
    diagnostics &sink_;
    model model_;
    std::vector<std::vector<const declaration *>> ast_declarations_;
    std::map<std::string, decl_ref> symbols_;
    std::map<std::string, decl_ref> handles_;
    std::map<std::string, size_align> layouts_;
    std::map<std::uint32_t, std::set<std::int64_t>> packet_ids_;
    std::set<std::string> layout_stack_;
    std::set<std::string> data_cycle_done_;
    std::set<std::string> data_cycle_stack_;
    /// Current resolve_type() recursion depth (recursion guard).
    std::uint32_t resolve_depth_{0};
};

} // namespace

auto analyze(std::vector<source_unit> units, diagnostics &sink)
    -> std::optional<model> {
    analyzer instance{std::move(units), sink};
    return instance.run();
}

} // namespace refrain::schema
