// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <schema/code_writer.hpp>
#include <schema/emit.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace refrain::schema {
namespace {

struct ridl_context {
    const model &program;
    std::string_view hash;
    /// slice element (module, decl) -> C slice struct name, ordered.
    std::map<std::pair<std::uint32_t, std::uint32_t>, std::string> slices;
};

[[nodiscard]] auto decl_of(const model &program, std::uint32_t module_index,
                           std::uint32_t decl_index) -> const decl_model & {
    return program.modules[module_index].declarations[decl_index];
}

// Forward declarations: the extension emitters below share the service-method
// signature/wrapper helpers defined further down.
[[nodiscard]] auto vtable_signature(const model &program,
                                    const method_model &method) -> std::string;
[[nodiscard]] auto cpp_method_definition(const model &program,
                                         const method_model &method)
    -> std::string;

[[nodiscard]] auto module_name(const model &program, std::uint32_t module_index)
    -> const std::string & {
    return program.modules[module_index].name;
}

[[nodiscard]] auto c_decl_name(const model &program, std::uint32_t module_index,
                               const decl_model &decl) -> std::string {
    return emit::abi_prefix(module_name(program, module_index)) + "_" +
           decl.name;
}

/// Fully qualified C++ spelling of a generated declaration, without the
/// leading `::`. For RIDL modules this names the generated alias; for RSC
/// modules it names the struct declared in the module header. Both spellings
/// denote the same underlying type, so `custom_traits` is specialized once.
[[nodiscard]] auto cpp_type_name(const model &program,
                                 std::uint32_t module_index,
                                 const decl_model &decl) -> std::string {
    return emit::cpp_namespace(module_name(program, module_index)) +
           "::" + emit::cpp_identifier(decl.name);
}

[[nodiscard]] auto cpp_decl_name(const model &program,
                                 std::uint32_t module_index,
                                 const decl_model &decl) -> std::string {
    return "::" + cpp_type_name(program, module_index, decl);
}

[[nodiscard]] auto handle_c_type(const model &program,
                                 const resolved_type &type) -> std::string {
    const auto &decl{decl_of(program, type.module_index, type.decl_index)};
    return emit::abi_prefix(module_name(program, type.module_index)) +
           "_handle_" + decl.name;
}

[[nodiscard]] auto handle_cpp_type(const model &program,
                                   const resolved_type &type) -> std::string {
    return cpp_decl_name(program, type.module_index,
                         decl_of(program, type.module_index, type.decl_index));
}

[[nodiscard]] auto slice_c_type(const model &program, const resolved_type &type)
    -> std::string {
    const auto &element{type.arguments.front()};
    const auto &decl{
        decl_of(program, element.module_index, element.decl_index)};
    return emit::abi_prefix(module_name(program, element.module_index)) +
           "_slice_" + decl.name;
}

[[nodiscard]] auto slice_element_cpp_type(const model &program,
                                          const resolved_type &type)
    -> std::string {
    const auto &element{type.arguments.front()};
    return cpp_decl_name(
        program, element.module_index,
        decl_of(program, element.module_index, element.decl_index));
}

[[nodiscard]] auto c_type(const model &program, const resolved_type &type)
    -> std::string {
    switch (type.kind) {
    case resolved_type::category::primitive:
        return std::string{emit::c_primitive_name(type.primitive)};
    case resolved_type::category::named:
        return c_decl_name(
            program, type.module_index,
            decl_of(program, type.module_index, type.decl_index));
    case resolved_type::category::handle:
        return handle_c_type(program, type);
    case resolved_type::category::slice:
        return slice_c_type(program, type);
    case resolved_type::category::array:
    case resolved_type::category::optional:
    case resolved_type::category::repeated:
    case resolved_type::category::map:
    case resolved_type::category::either:
    case resolved_type::category::holder:
        std::unreachable(); // sema rejects container types in the C ABI
    }
    std::unreachable();
}

[[nodiscard]] auto cpp_type(const model &program, const resolved_type &type)
    -> std::string {
    switch (type.kind) {
    case resolved_type::category::primitive:
        return std::string{emit::cpp_primitive_name(type.primitive)};
    case resolved_type::category::named:
        return cpp_decl_name(
            program, type.module_index,
            decl_of(program, type.module_index, type.decl_index));
    case resolved_type::category::handle:
        return handle_cpp_type(program, type);
    case resolved_type::category::slice:
        return "std::span<const " + slice_element_cpp_type(program, type) + ">";
    case resolved_type::category::array:
    case resolved_type::category::optional:
    case resolved_type::category::repeated:
    case resolved_type::category::map:
    case resolved_type::category::either:
    case resolved_type::category::holder:
        std::unreachable(); // sema rejects container types in the C ABI
    }
    std::unreachable();
}

[[nodiscard]] auto module_last_segment(std::string_view module) -> std::string {
    const auto segments{emit::split_module(module)};
    return segments.empty() ? std::string{} : segments.back();
}

[[nodiscard]] auto service_prefix(const model &program,
                                  std::uint32_t module_index,
                                  const decl_model &decl) -> std::string {
    auto prefix{emit::abi_prefix(module_name(program, module_index))};
    if (module_last_segment(module_name(program, module_index)) != decl.name) {
        prefix += '_';
        prefix += decl.name;
    }
    return prefix;
}

[[nodiscard]] auto extension_c_prefix(const model &program,
                                      std::uint32_t module_index,
                                      const decl_model &decl) -> std::string {
    return emit::abi_prefix(module_name(program, module_index)) + "_" +
           decl.name;
}

/// True when the module's generated C header needs the shared extension-ref
/// block: it declares an extension or hosts a service that declares extension
/// points.
[[nodiscard]] auto module_uses_extension_abi(const module_model &module)
    -> bool {
    return std::ranges::any_of(module.declarations, [](const decl_model &decl) {
        return decl.kind == declaration_kind::extension ||
               (decl.kind == declaration_kind::service &&
                !decl.extension_points.empty());
    });
}

[[nodiscard]] auto service_id_of(const model &program,
                                 std::uint32_t module_index,
                                 const decl_model &decl) -> std::string {
    if (!decl.service.provides.empty()) {
        return decl.service.provides.front();
    }
    return module_name(program, module_index);
}

/// `refrain::services::sim` -> `refrain/services/sim`, the path component of a
/// stable event id.
[[nodiscard]] auto module_slash_path(std::string_view module) -> std::string {
    std::string result;
    for (const auto &segment : emit::split_module(module)) {
        if (!result.empty()) {
            result += '/';
        }
        result += segment;
    }
    return result;
}

/// Stable event identity: `refrain:event/<module '/' separated>/<Event>`.
[[nodiscard]] auto event_id_text(const model &program,
                                 std::uint32_t module_index,
                                 const decl_model &decl) -> std::string {
    return "refrain:event/" +
           module_slash_path(module_name(program, module_index)) + "/" +
           decl.name;
}

/// C++ spelling of an event field type. Sema restricts events to primitives and
/// `slice<scalar>`, so the contract-struct slice helpers (which require a named
/// element declaration) do not apply.
[[nodiscard]] auto event_cpp_type(const resolved_type &type) -> std::string {
    switch (type.kind) {
    case resolved_type::category::primitive:
        return std::string{emit::cpp_primitive_name(type.primitive)};
    case resolved_type::category::slice: {
        const auto &element{type.arguments.front()};
        return "std::span<const " +
               std::string{emit::cpp_primitive_name(element.primitive)} + ">";
    }
    case resolved_type::category::named:
    case resolved_type::category::handle:
    case resolved_type::category::array:
    case resolved_type::category::optional:
    case resolved_type::category::repeated:
    case resolved_type::category::map:
    case resolved_type::category::either:
    case resolved_type::category::holder:
        std::unreachable(); // sema rejects these event field types
    }
    std::unreachable();
}

/// Event struct in the module header: deterministic field order, explicit
/// `reserved_N` padding (the contract-struct algorithm without the leading
/// `struct_size` word) and size/align/offset assertions, followed by the
/// stable id constant.
[[nodiscard]] auto emit_event_cpp(const model &program,
                                  std::uint32_t module_index,
                                  const decl_model &decl) -> std::string {
    code_writer writer;
    writer.raw(emit::doc_block(decl.documentation, "/// "));
    writer.line("struct {} {{", decl.name);
    std::uint32_t cursor{0};
    std::size_t padding_index{0};
    for (const auto &field : decl.fields) {
        if (field.abi_offset > cursor) {
            writer.line("    std::uint8_t reserved_{}[{}];", padding_index,
                        field.abi_offset - cursor);
            ++padding_index;
            cursor = field.abi_offset;
        }
        writer.raw(emit::doc_block(field.documentation, "    /// "));
        writer.line("    {} {};", event_cpp_type(field.type), field.name);
        cursor = field.abi_offset + field.abi_size;
    }
    if (decl.abi_size > cursor) {
        writer.line("    std::uint8_t reserved_{}[{}];", padding_index,
                    decl.abi_size - cursor);
    }
    writer.line("}};");
    writer.blank();
    writer.line("static_assert(sizeof({}) == {});", decl.name, decl.abi_size);
    writer.line("static_assert(alignof({}) == {});", decl.name,
                decl.abi_alignment);
    for (const auto &field : decl.fields) {
        writer.line("static_assert(offsetof({}, {}) == {});", decl.name,
                    field.name, field.abi_offset);
    }
    writer.blank();
    writer.line("inline constexpr std::string_view {}_id =", decl.name);
    writer.line("    \"{}\";",
                emit::escape_text(event_id_text(program, module_index, decl)));
    writer.blank();
    return writer.take();
}

/// `event_traits<T>` specializations. They must live outside the module
/// namespace (they specialize a template in `refrain::contracts`), so the
/// emitter writes them after the module namespace closes.
[[nodiscard]] auto emit_event_traits(const model &program,
                                     std::uint32_t module_index)
    -> std::string {
    const auto &module{program.modules[module_index]};
    const auto has_events{
        std::ranges::any_of(module.declarations, [](const decl_model &decl) {
            return decl.kind == declaration_kind::event;
        })};
    if (!has_events) {
        return {};
    }
    code_writer writer;
    writer.line("namespace refrain::contracts {{");
    writer.blank();
    for (const auto &decl : module.declarations) {
        if (decl.kind != declaration_kind::event) {
            continue;
        }
        const auto type{cpp_decl_name(program, module_index, decl)};
        writer.line("template <> struct event_traits<{}> {{", type);
        writer.line("    static constexpr bool is_event = true;");
        writer.line("    static constexpr std::string_view id =");
        writer.line("        {}_id;", type);
        writer.line("    static constexpr bool batch = {};",
                    decl.event_batch ? "true" : "false");
        writer.line("    static constexpr bool cancellable = {};",
                    decl.event_cancellable ? "true" : "false");
        writer.line("}};");
        writer.blank();
    }
    writer.line("}} // namespace refrain::contracts");
    writer.blank();
    return writer.take();
}

[[nodiscard]] auto join(const std::vector<std::string> &values,
                        std::string_view separator) -> std::string {
    std::string result;
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (index != 0) {
            result += separator;
        }
        result += values[index];
    }
    return result;
}

[[nodiscard]] auto c_side_macro(std::string_view side) -> std::string {
    if (side == "server") {
        return "REFRAIN_SIDE_SERVER";
    }
    if (side == "client") {
        return "REFRAIN_SIDE_CLIENT";
    }
    return "REFRAIN_SIDE_BOTH";
}

[[nodiscard]] auto c_thread_macro(std::string_view thread) -> std::string {
    if (thread == "main") {
        return "REFRAIN_THREAD_MAIN";
    }
    if (thread == "sim") {
        return "REFRAIN_THREAD_SIM";
    }
    if (thread == "render") {
        return "REFRAIN_THREAD_RENDER";
    }
    if (thread == "worker") {
        return "REFRAIN_THREAD_WORKER";
    }
    if (thread == "io") {
        return "REFRAIN_THREAD_IO";
    }
    if (thread == "net") {
        return "REFRAIN_THREAD_NET";
    }
    return "REFRAIN_THREAD_ANY";
}

[[nodiscard]] auto cpp_side_name(std::string_view side) -> std::string {
    if (side == "server") {
        return "server";
    }
    if (side == "client") {
        return "client";
    }
    return "both";
}

[[nodiscard]] auto cpp_thread_name(std::string_view thread) -> std::string {
    if (thread == "main") {
        return "main";
    }
    if (thread == "sim") {
        return "sim";
    }
    if (thread == "render") {
        return "render";
    }
    if (thread == "worker") {
        return "worker";
    }
    if (thread == "io") {
        return "io";
    }
    if (thread == "net") {
        return "net";
    }
    return "any";
}

/// C macro for a contract composition mode. Sema validated the enum, so an
/// unknown value means sema/emitter drift; fall back to the default
/// (exclusive) rather than emitting an undefined identifier.
[[nodiscard]] auto c_composition_macro(std::string_view composition)
    -> std::string {
    if (composition == "instanced") {
        return "REFRAIN_COMPOSITION_INSTANCED";
    }
    if (composition == "chain") {
        return "REFRAIN_COMPOSITION_CHAIN";
    }
    if (composition == "registry") {
        return "REFRAIN_COMPOSITION_REGISTRY";
    }
    if (composition == "sealed") {
        return "REFRAIN_COMPOSITION_SEALED";
    }
    return "REFRAIN_COMPOSITION_EXCLUSIVE";
}

[[nodiscard]] auto cpp_composition_name(std::string_view composition)
    -> std::string {
    if (composition == "instanced") {
        return "instanced";
    }
    if (composition == "chain") {
        return "chain";
    }
    if (composition == "registry") {
        return "registry";
    }
    if (composition == "sealed") {
        return "sealed";
    }
    return "exclusive";
}

void collect_type_dependencies(const model &program, const resolved_type &type,
                               std::set<std::uint32_t> &modules) {
    if (type.kind == resolved_type::category::named ||
        type.kind == resolved_type::category::handle) {
        modules.insert(type.module_index);
    }
    for (const auto &argument : type.arguments) {
        collect_type_dependencies(program, argument, modules);
    }
}

[[nodiscard]] auto module_dependencies(const model &program,
                                       std::uint32_t module_index)
    -> std::vector<std::uint32_t> {
    std::set<std::uint32_t> modules;
    const auto &module{program.modules[module_index]};
    for (const auto &decl : module.declarations) {
        for (const auto &field : decl.fields) {
            collect_type_dependencies(program, field.type, modules);
        }
        for (const auto &method : decl.methods) {
            for (const auto &parameter : method.parameters) {
                collect_type_dependencies(program, parameter.type, modules);
            }
            if (method.can_fail) {
                collect_type_dependencies(program, method.error_type, modules);
            }
            collect_type_dependencies(program, method.result_type, modules);
        }
    }
    modules.erase(module_index);
    return {modules.begin(), modules.end()};
}

void collect_slice_types(const model &program, const resolved_type &type,
                         ridl_context &context) {
    if (type.kind == resolved_type::category::slice) {
        const auto &element{type.arguments.front()};
        const auto key{std::pair{element.module_index, element.decl_index}};
        const auto &decl{
            decl_of(program, element.module_index, element.decl_index)};
        context.slices.emplace(
            key, emit::abi_prefix(module_name(program, element.module_index)) +
                     "_slice_" + decl.name);
    }
    for (const auto &argument : type.arguments) {
        collect_slice_types(program, argument, context);
    }
}

[[nodiscard]] auto is_struct(const decl_model &decl) noexcept -> bool {
    return decl.kind == declaration_kind::structure;
}

/// Struct-like declarations carry schema fields (and therefore reflection
/// metadata); enums/flags/handles/services are not records.
[[nodiscard]] auto is_reflectable_decl(const decl_model &decl) noexcept
    -> bool {
    switch (decl.kind) {
    case declaration_kind::structure:
    case declaration_kind::record:
    case declaration_kind::packet:
        return true;
    case declaration_kind::enumeration:
    case declaration_kind::flags:
    case declaration_kind::handle:
    case declaration_kind::service:
    case declaration_kind::callback:
    // Events are in-process values with their own `event_traits` metadata;
    // they are not reflectable contract records.
    case declaration_kind::event:
    // Extensions expose their own `custom_extension_traits` metadata.
    case declaration_kind::extension:
        return false;
    }
    return false;
}

/// Topological order of contract structs (by-value dependencies first).
void topo_visit(const model &program, std::uint32_t module_index,
                std::uint32_t decl_index, std::vector<bool> &visited,
                std::vector<std::uint32_t> &order) {
    if (visited[decl_index]) {
        return;
    }
    visited[decl_index] = true;
    const auto &decl{decl_of(program, module_index, decl_index)};
    for (const auto &field : decl.fields) {
        if (field.type.kind != resolved_type::category::named) {
            continue;
        }
        const auto &dependency{
            decl_of(program, field.type.module_index, field.type.decl_index)};
        if (!is_struct(dependency)) {
            continue;
        }
        if (field.type.module_index != module_index) {
            continue; // cross-module definitions come from an #include
        }
        topo_visit(program, module_index, field.type.decl_index, visited,
                   order);
    }
    order.push_back(decl_index);
}

[[nodiscard]] auto struct_order(const model &program,
                                std::uint32_t module_index)
    -> std::vector<std::uint32_t> {
    const auto &module{program.modules[module_index]};
    std::vector<bool> visited(module.declarations.size(), false);
    std::vector<std::uint32_t> order;
    for (std::uint32_t decl_index = 0; decl_index < module.declarations.size();
         ++decl_index) {
        if (is_struct(module.declarations[decl_index])) {
            topo_visit(program, module_index, decl_index, visited, order);
        }
    }
    return order;
}

[[nodiscard]] auto emit_enum_abi(const model &program,
                                 std::uint32_t module_index,
                                 const decl_model &decl) -> std::string {
    code_writer writer;
    writer.raw(emit::doc_block(decl.documentation, "// "));
    const auto c_name{c_decl_name(program, module_index, decl)};
    const auto macro_prefix{emit::upper(c_name)};
    writer.line("typedef {} {}; // {} '{}'", emit::c_underlying_name(decl),
                c_name, declaration_kind_name(decl.kind), decl.name);
    for (const auto &entry : decl.entries) {
        writer.line("#define {}_{} (({})({}))", macro_prefix,
                    emit::upper(entry.name), c_name, entry.value.value_or(0));
    }
    writer.blank();
    return writer.take();
}

[[nodiscard]] auto emit_enum_cpp(const decl_model &decl) -> std::string {
    code_writer writer;
    writer.raw(emit::doc_block(decl.documentation, "/// "));
    const auto kind_name{decl.kind == declaration_kind::flags ? "flags"
                                                              : "enum"};
    writer.line("enum class {} : {} {{ // {} '{}'", decl.name,
                emit::cpp_underlying_name(decl), kind_name, decl.name);
    for (const auto &entry : decl.entries) {
        writer.raw(emit::doc_block(entry.documentation, "    /// "));
        writer.line("    {} = {},", entry.name, entry.value.value_or(0));
    }
    writer.line("}};");
    if (decl.kind == declaration_kind::flags) {
        const auto underlying{emit::cpp_underlying_name(decl)};
        writer.line(
            "\nconstexpr auto operator|({0} lhs, {0} rhs) noexcept -> {0} {{\n"
            "    return static_cast<{0}>(static_cast<{1}>(lhs) | "
            "static_cast<{1}>(rhs));\n"
            "}}\n"
            "constexpr auto operator&({0} lhs, {0} rhs) noexcept -> {0} {{\n"
            "    return static_cast<{0}>(static_cast<{1}>(lhs) & "
            "static_cast<{1}>(rhs));\n"
            "}}\n"
            "constexpr auto operator~({0} value) noexcept -> {0} {{\n"
            "    return static_cast<{0}>(~static_cast<{1}>(value));\n"
            "}}",
            decl.name, underlying);
    }
    writer.blank();
    return writer.take();
}

/// One extension's C++ wrapper: stable metadata constants plus direct
/// vtable dispatch. The reference is validated once by
/// `provider::extension<T>()`; no check happens per call.
[[nodiscard]] auto emit_extension_cpp(const model &program,
                                      std::uint32_t module_index,
                                      const decl_model &decl) -> std::string {
    code_writer writer;
    const auto c_vtable{extension_c_prefix(program, module_index, decl) +
                        "_vtable"};
    writer.raw(emit::doc_block(decl.documentation, "/// "));
    writer.line("/// Provider-implemented extension extending '{}'.",
                emit::escape_text(decl.extension.extends_service));
    writer.line("/// A host binds the reference once through "
                "`provider::extension<T>()`;");
    writer.line("/// method calls below dispatch through the bound vtable.");
    writer.line("class {} final {{", decl.name);
    writer.line("  public:");
    writer.line("    using vtable = ::{};", c_vtable);
    writer.line("    static constexpr std::string_view extension_id =");
    writer.line("        \"{}\";", emit::escape_text(decl.extension_id));
    writer.line("    static constexpr std::string_view extends_service =");
    writer.line("        \"{}\";",
                emit::escape_text(decl.extension.extends_service));
    writer.line("    static constexpr std::string_view capability =");
    writer.line("        \"{}\";",
                emit::escape_text(decl.extension.capability));
    writer.line("    static constexpr std::uint32_t abi_major = {};",
                decl.extension_version_major);
    writer.line("    static constexpr std::uint32_t abi_minor = {};",
                decl.extension_version_minor);
    writer.line("    static constexpr std::uint32_t abi_patch = {};",
                decl.extension_version_patch);
    writer.line("    /// Packed minimum version requested from a provider:");
    writer.line("    /// (major << 16) | minor.");
    writer.line("    static constexpr std::uint32_t abi_version =");
    writer.line("        ({}u << 16u) | {}u;", decl.extension_version_major,
                decl.extension_version_minor);
    writer.blank();
    writer.line("    {}(const vtable &table, void *self) noexcept", decl.name);
    writer.line("        : table_{{&table}}, self_{{self}} {{}}");
    writer.blank();
    writer.line("    [[nodiscard]] auto table() const noexcept");
    writer.line("        -> const vtable & {{");
    writer.line("        return *table_;");
    writer.line("    }}");
    writer.blank();
    writer.line("    [[nodiscard]] auto self() const noexcept -> void * {{");
    writer.line("        return self_;");
    writer.line("    }}");
    for (const auto &method : decl.methods) {
        writer.raw(cpp_method_definition(program, method));
    }
    writer.blank();
    writer.line("  private:");
    writer.line("    const vtable *table_;");
    writer.line("    void *self_;");
    writer.line("}};");
    writer.blank();
    writer.line("static_assert(offsetof(::{}, struct_size) == 0);", c_vtable);
    writer.blank();
    return writer.take();
}

/// Provider-side registration/query machinery for one service with extension
/// points. The generated code owns id/version/struct-size dispatch so a
/// provider only fills a binding table and forwards the vtable slot.
[[nodiscard]] auto emit_extension_support_cpp() -> std::string {
    return R"(namespace detail {
[[nodiscard]] inline auto extension_failure(::refrain_status status,
                                           std::string_view id)
    -> ::refrain::error {
    switch (status) {
    case REFRAIN_EXTENSION_NOT_FOUND:
        return ::refrain::make_error(
            ::refrain::error_code::not_found,
            std::string{"extension '"} + std::string{id} + "' is not provided");
    case REFRAIN_EXTENSION_VERSION_MISMATCH:
        return ::refrain::make_error(
            ::refrain::error_code::version_mismatch,
            std::string{"extension '"} + std::string{id} +
                "' does not provide a compatible version");
    case REFRAIN_EXTENSION_INVALID:
    default:
        return ::refrain::make_error(
            ::refrain::error_code::invalid_argument,
            std::string{"extension '"} + std::string{id} +
                "' returned a malformed reference");
    }
}
} // namespace detail

/// One extension a provider offers. Build it with make_extension_binding().
struct extension_binding {
    std::string_view id{};
    std::uint32_t abi_major{0};
    std::uint32_t abi_minor{0};
    std::uint32_t abi_patch{0};
    const void *vtable{nullptr};
    void *self{nullptr};
    std::uint32_t vtable_size{0};
};

/// Stable-id registry over a provider's extension bindings. The bindings and
/// the referenced vtables are owned by the base provider; extensions have no
/// independent lifetime.
class extension_registry final {
  public:
    constexpr explicit extension_registry(
        std::span<const extension_binding> entries) noexcept
        : entries_{entries} {}

    [[nodiscard]] auto query(::refrain_str id, std::uint32_t min_version,
                             ::refrain_extension_ref &out) const noexcept
        -> ::refrain_status {
        if (out.struct_size != REFRAIN_EXTENSION_REF_SIZE) {
            return REFRAIN_EXTENSION_INVALID;
        }
        const auto requested_major{min_version >> 16u};
        const auto requested_minor{min_version & 0xFFFFu};
        const auto wanted{::refrain::contracts::as_string_view(id)};
        for (const auto &entry : entries_) {
            if (entry.id != wanted) {
                continue;
            }
            if (entry.vtable == nullptr || entry.vtable_size == 0) {
                return REFRAIN_EXTENSION_INVALID;
            }
            if (entry.abi_major != requested_major ||
                entry.abi_minor < requested_minor) {
                return REFRAIN_EXTENSION_VERSION_MISMATCH;
            }
            out.abi_major = entry.abi_major;
            out.abi_minor = entry.abi_minor;
            out.abi_patch = entry.abi_patch;
            out.id = ::refrain::contracts::to_borrowed(entry.id);
            out.vtable = entry.vtable;
            out.self = entry.self;
            return REFRAIN_STATUS_OK;
        }
        return REFRAIN_EXTENSION_NOT_FOUND;
    }

  private:
    std::span<const extension_binding> entries_;
};

/// Build one registration entry for `Extension`. A vtable whose struct_size is
/// below the generated size is stored as null and fails closed at query time.
template <typename Extension>
[[nodiscard]] constexpr auto make_extension_binding(
    const typename Extension::vtable &table, void *self) noexcept
    -> extension_binding {
    constexpr auto expected_size{
        static_cast<std::uint32_t>(sizeof(typename Extension::vtable))};
    const auto valid{table.struct_size >= expected_size};
    return extension_binding{
        .id = Extension::extension_id,
        .abi_major = Extension::abi_major,
        .abi_minor = Extension::abi_minor,
        .abi_patch = Extension::abi_patch,
        .vtable = valid ? static_cast<const void *>(&table) : nullptr,
        .self = self,
        .vtable_size = expected_size,
    };
}

/// Vtable adapter: assign `&query_extension<Provider>` to the service's
/// `query_extension` slot. `Provider` must expose a public
/// `extension_registry extensions;` member holding the bindings above.
template <typename Provider>
[[nodiscard]] auto query_extension(void *self, ::refrain_str id,
                                   std::uint32_t min_version,
                                   ::refrain_extension_ref *out) noexcept
    -> ::refrain_status {
    if (self == nullptr || out == nullptr) {
        return REFRAIN_EXTENSION_INVALID;
    }
    return static_cast<const Provider *>(self)->extensions.query(
        id, min_version, *out);
}
)";
}

/// `provider::extension<T>()` body. Uses the `service_vtable` alias emitted by
/// the caller so the fixed block needs no per-service interpolation.
[[nodiscard]] auto emit_provider_extension_api() -> std::string {
    return R"(
    /// Bind one extension declared by this service. The stable id, version
    /// and struct sizes are validated once here; the returned wrapper calls
    /// the extension vtable directly.
    template <typename Extension>
    [[nodiscard]] auto extension() const
        -> std::expected<Extension, ::refrain::error> {
        const auto slot_end{static_cast<std::uint32_t>(
            offsetof(service_vtable, query_extension) +
            sizeof(table_->query_extension))};
        if (table_->struct_size < slot_end ||
            table_->query_extension == nullptr) {
            return std::unexpected(::refrain::make_error(
                ::refrain::error_code::unsupported,
                "service provider does not expose extension queries"));
        }
        ::refrain_extension_ref ref{};
        ref.struct_size = REFRAIN_EXTENSION_REF_SIZE;
        const auto status{table_->query_extension(
            self_, ::refrain::contracts::to_borrowed(Extension::extension_id),
            Extension::abi_version, &ref)};
        if (status != REFRAIN_STATUS_OK) {
            return std::unexpected(
                detail::extension_failure(status, Extension::extension_id));
        }
        if (ref.struct_size != REFRAIN_EXTENSION_REF_SIZE ||
            ref.vtable == nullptr ||
            ::refrain::contracts::as_string_view(ref.id) !=
                Extension::extension_id ||
            ref.abi_major != Extension::abi_major ||
            ref.abi_minor < Extension::abi_minor) {
            return std::unexpected(::refrain::make_error(
                ::refrain::error_code::invalid_argument,
                std::string{"extension '"} +
                    std::string{Extension::extension_id} +
                    "' returned a malformed reference"));
        }
        const auto *extension_table{static_cast<
            const typename Extension::vtable *>(ref.vtable)};
        if (extension_table->struct_size <
            static_cast<std::uint32_t>(
                sizeof(typename Extension::vtable))) {
            return std::unexpected(::refrain::make_error(
                ::refrain::error_code::invalid_argument,
                std::string{"extension '"} +
                    std::string{Extension::extension_id} +
                    "' has an undersized vtable"));
        }
        return Extension{*extension_table, ref.self};
    }
)";
}

[[nodiscard]] auto emit_struct_abi(const model &program,
                                   std::uint32_t module_index,
                                   const decl_model &decl) -> std::string {
    code_writer writer;
    const auto c_name{c_decl_name(program, module_index, decl)};
    writer.raw(emit::doc_block(decl.documentation, "// "));
    writer.raw(emit::attribute_comment(decl.attributes));
    writer.line("struct {} {{", c_name);
    std::uint32_t cursor{0};
    writer.line("    uint32_t struct_size;");
    cursor = 4;
    std::size_t padding_index{0};
    for (const auto &field : decl.fields) {
        if (field.abi_offset > cursor) {
            writer.line("    uint8_t reserved_{}[{}];", padding_index,
                        field.abi_offset - cursor);
            ++padding_index;
            cursor = field.abi_offset;
        }
        writer.raw(emit::doc_block(field.documentation, "    // "));
        writer.raw(emit::attribute_comment(field.attributes, "    "));
        writer.line("    {} {};", c_type(program, field.type), field.name);
        cursor = field.abi_offset + field.abi_size;
    }
    if (decl.abi_size > cursor) {
        writer.line("    uint8_t reserved_{}[{}];", padding_index,
                    decl.abi_size - cursor);
    }
    writer.line("}}; // {} size = {}", c_name, decl.abi_size);
    writer.line("#define {}_SIZE ((uint32_t)sizeof({}))", emit::upper(c_name),
                c_name);
    writer.blank();
    return writer.take();
}

/// Shared extension-reference ABI. Emitted into every module header that
/// declares an extension or hosts extension points and guarded so an aggregate
/// translation unit including several such headers sees one definition.
void emit_extension_abi_preamble(code_writer &writer) {
    writer.raw(
        R"(// Extension ABI v1: a service exposes provider-implemented extensions
// through its `query_extension` vtable slot. A reference is a
// struct_size-prologue value; the vtable pointer stays type-erased in C and is
// typed by the generated C++ wrapper.
#ifndef REFRAIN_EXTENSION_ABI_DEFINED
#define REFRAIN_EXTENSION_ABI_DEFINED

#define REFRAIN_EXTENSION_ABI_MAJOR 1u
#define REFRAIN_EXTENSION_ABI_MINOR 0u

// Query status values; the numeric codes mirror the shared core error taxonomy
// (invalid_argument, not_found, version_mismatch).
#define REFRAIN_EXTENSION_INVALID ((refrain_status)1u)
#define REFRAIN_EXTENSION_NOT_FOUND ((refrain_status)2u)
#define REFRAIN_EXTENSION_VERSION_MISMATCH ((refrain_status)8u)

// Packed minimum extension version requested by a query:
// (major << 16) | minor.
#define REFRAIN_EXTENSION_VERSION(major, minor)                                \
    ((((uint32_t)(major)) << 16u) | ((uint32_t)(minor) & 0xFFFFu))

typedef struct refrain_extension_ref {
    // sizeof(refrain_extension_ref) written by the producer; a host accepts
    // only its own compiled size.
    uint32_t struct_size;
    uint32_t abi_major;
    uint32_t abi_minor;
    uint32_t abi_patch;
    // Stable id derived from the parent service id and the declaration name.
    refrain_str id;
    // Provider-owned extension vtable (first member: uint32_t struct_size).
    const void *vtable;
    // Provider-owned extension instance passed to every vtable method.
    void *self;
} refrain_extension_ref;

#define REFRAIN_EXTENSION_REF_SIZE ((uint32_t)sizeof(refrain_extension_ref))
#endif // REFRAIN_EXTENSION_ABI_DEFINED

)");
}

/// One extension's C vtable and stable metadata macros.
[[nodiscard]] auto emit_extension_abi(const model &program,
                                      std::uint32_t module_index,
                                      const decl_model &decl) -> std::string {
    code_writer writer;
    const auto prefix{extension_c_prefix(program, module_index, decl)};
    const auto upper_prefix{emit::upper(prefix)};
    writer.raw(emit::doc_block(decl.documentation, "// "));
    writer.line("#define {}_ID \"{}\"", upper_prefix,
                emit::escape_text(decl.extension_id));
    writer.line("#define {}_ABI_MAJOR {}u", upper_prefix,
                decl.extension_version_major);
    writer.line("#define {}_ABI_MINOR {}u", upper_prefix,
                decl.extension_version_minor);
    writer.line("#define {}_ABI_PATCH {}u", upper_prefix,
                decl.extension_version_patch);
    writer.line("#define {}_CAPABILITY \"{}\"", upper_prefix,
                emit::escape_text(decl.extension.capability));
    writer.line("#define {}_EXTENDS \"{}\"", upper_prefix,
                emit::escape_text(decl.extension.extends_service));
    writer.blank();
    writer.line("typedef struct {}_vtable {{", prefix);
    writer.line("    uint32_t struct_size;");
    for (const auto &method : decl.methods) {
        writer.raw(emit::doc_block(method.documentation, "    // "));
        writer.raw(emit::attribute_comment(method.attributes, "    "));
        writer.line("    {};", vtable_signature(program, method));
    }
    writer.line("}} {}_vtable; // extension {}", prefix,
                emit::escape_text(decl.extension_id));
    writer.blank();
    writer.line("#define {}_VTABLE_SIZE ((uint32_t)sizeof({}_vtable))",
                upper_prefix, prefix);
    writer.blank();
    return writer.take();
}

/// Service-side extension ABI: stable ids and the query slot appended after
/// the contract method slots. The slot is only emitted for services that
/// declare extension points, so services without extensions keep their exact
/// previous vtable layout.
void emit_service_extension_abi(code_writer &writer, const model &program,
                                std::uint32_t module_index,
                                const decl_model &decl) {
    const auto prefix{service_prefix(program, module_index, decl)};
    const auto upper_prefix{emit::upper(prefix)};
    std::vector<std::string> ids;
    ids.reserve(decl.extension_points.size());
    for (const auto &point : decl.extension_points) {
        ids.push_back(decl_of(program, point.module_index, point.decl_index)
                          .extension_id);
    }
    writer.line("#define {}_EXTENSION_IDS \"{}\"", upper_prefix,
                emit::escape_text(join(ids, ",")));
}

void emit_service_query_slot(code_writer &writer, const decl_model &decl) {
    if (decl.extension_points.empty()) {
        return;
    }
    writer.raw(
        "    // Extension query. Returns REFRAIN_EXTENSION_NOT_FOUND when the\n"
        "    // provider does not offer `id`, "
        "REFRAIN_EXTENSION_VERSION_MISMATCH\n"
        "    // when its major/minor cannot satisfy `min_version`, and\n"
        "    // REFRAIN_EXTENSION_INVALID for a malformed reference. The host\n"
        "    // initialises `out->struct_size` before the call.\n"
        "    refrain_status (*query_extension)(void *self, refrain_str id,\n"
        "                                      uint32_t min_version,\n"
        "                                      refrain_extension_ref *out);\n");
}

[[nodiscard]] auto vtable_signature(const model &program,
                                    const method_model &method) -> std::string {
    std::string parameters{"void *self"};
    for (const auto &parameter : method.parameters) {
        if (parameter.out) {
            parameters += ", ";
            parameters += std::format("{} *{}", c_type(program, parameter.type),
                                      parameter.name);
        } else if (parameter.type.kind == resolved_type::category::named &&
                   is_struct(decl_of(program, parameter.type.module_index,
                                     parameter.type.decl_index))) {
            parameters += ", ";
            parameters +=
                std::format("const {} *{}", c_type(program, parameter.type),
                            parameter.name);
        } else {
            parameters += ", ";
            parameters += std::format("{} {}", c_type(program, parameter.type),
                                      parameter.name);
        }
    }
    if (method.can_fail && !method.result_type.is_void()) {
        parameters +=
            std::format(", {} *out_value", c_type(program, method.result_type));
    }
    const auto return_type{method.can_fail
                               ? std::string{"refrain_status"}
                               : c_type(program, method.result_type)};
    return std::format("{} (*{})({})", return_type, method.name, parameters);
}

[[nodiscard]] auto cpp_method_declaration(const model &program,
                                          const method_model &method)
    -> std::string {
    std::string parameters;
    for (std::size_t index = 0; index < method.parameters.size(); ++index) {
        const auto &parameter{method.parameters[index]};
        if (index != 0) {
            parameters += ", ";
        }
        if (parameter.out) {
            parameters +=
                cpp_type(program, parameter.type) + " &" + parameter.name;
        } else if (parameter.type.kind == resolved_type::category::named &&
                   is_struct(decl_of(program, parameter.type.module_index,
                                     parameter.type.decl_index))) {
            parameters += "const " + cpp_type(program, parameter.type) + " &" +
                          parameter.name;
        } else {
            parameters +=
                cpp_type(program, parameter.type) + " " + parameter.name;
        }
    }
    std::string return_type;
    if (method.can_fail) {
        const auto error{cpp_type(program, method.error_type)};
        if (method.result_type.is_void()) {
            return_type = "std::expected<void, " + error + ">";
        } else {
            return_type = "std::expected<" +
                          cpp_type(program, method.result_type) + ", " + error +
                          ">";
        }
    } else {
        return_type = cpp_type(program, method.result_type);
    }
    // `[[nodiscard]]` is meaningless (and rejected by GCC as an attribute on a
    // `void` function) for a plain `void` method; a `#[schedule]` target may
    // legitimately be `fn maintenance() -> void`. `result<void, E>` still
    // returns an expected, so it keeps the attribute.
    if (!method.can_fail && method.result_type.is_void()) {
        return std::format("auto {}({}) const -> {}", method.name, parameters,
                           return_type);
    }
    return std::format("[[nodiscard]] auto {}({}) const -> {}", method.name,
                       parameters, return_type);
}

[[nodiscard]] auto cpp_return_conversion(const model &program,
                                         const resolved_type &type,
                                         std::string_view expression)
    -> std::string {
    switch (type.kind) {
    case resolved_type::category::named:
        if (decl_of(program, type.module_index, type.decl_index).kind ==
            declaration_kind::structure) {
            return std::string{expression};
        }
        return std::format("static_cast<{}>({})", cpp_type(program, type),
                           expression);
    case resolved_type::category::primitive:
        switch (type.primitive) {
        case primitive_kind::void_type:
            return {};
        case primitive_kind::boolean:
            return std::format("({} != 0)", expression);
        case primitive_kind::string:
            return std::format("::refrain::contracts::as_string_view({})",
                               expression);
        case primitive_kind::bytes:
            return std::format("::refrain::contracts::as_bytes({})",
                               expression);
        case primitive_kind::mut_bytes:
            return std::format("std::span<std::uint8_t>{{{}.data, "
                               "static_cast<std::size_t>({}.len)}}",
                               expression, expression);
        default:
            return std::format("static_cast<{}>({})", cpp_type(program, type),
                               expression);
        }
    case resolved_type::category::handle:
        return std::format("{}{{{}}}", cpp_type(program, type), expression);
    case resolved_type::category::slice:
        return std::format("std::span<const {}>{{{}.data, "
                           "static_cast<std::size_t>({}.len)}}",
                           slice_element_cpp_type(program, type), expression,
                           expression);
    case resolved_type::category::array:
    case resolved_type::category::optional:
    case resolved_type::category::repeated:
    case resolved_type::category::map:
    case resolved_type::category::either:
    case resolved_type::category::holder:
        std::unreachable(); // sema rejects container return types
    }
    std::unreachable();
}

[[nodiscard]] auto cpp_arg_expression(const model &program,
                                      const field_model &parameter)
    -> std::string {
    const auto &type{parameter.type};
    switch (type.kind) {
    case resolved_type::category::named:
        if (parameter.out) {
            // `out` structs are initialized in place; pass the caller's object.
            return "&" + parameter.name;
        }
        if (is_struct(decl_of(program, type.module_index, type.decl_index))) {
            // The wrapper method body creates `abi_<param>` (a copy whose
            // mandatory `struct_size` is filled in) before the call, so a
            // caller that forgot init_struct() cannot send struct_size == 0.
            return "&abi_" + parameter.name;
        }
        return std::format("static_cast<{}>({})", c_type(program, type),
                           parameter.name);
    case resolved_type::category::primitive:
        switch (type.primitive) {
        case primitive_kind::string:
        case primitive_kind::bytes:
            return "::refrain::contracts::to_borrowed(" + parameter.name + ")";
        case primitive_kind::mut_bytes:
            return "::refrain::contracts::to_mut(" + parameter.name + ")";
        case primitive_kind::boolean:
            return "static_cast<std::uint8_t>(" + parameter.name + ")";
        case primitive_kind::void_type:
            return {};
        default:
            return parameter.name;
        }
    case resolved_type::category::handle:
        return parameter.name + ".raw()";
    case resolved_type::category::slice:
        return std::format(
            "{}{{{}.data(), static_cast<std::uint64_t>({}.size())}}",
            slice_c_type(program, type), parameter.name, parameter.name);
    case resolved_type::category::array:
    case resolved_type::category::optional:
    case resolved_type::category::repeated:
    case resolved_type::category::map:
    case resolved_type::category::either:
    case resolved_type::category::holder:
        std::unreachable(); // sema rejects container parameter types
    }
    std::unreachable();
}

[[nodiscard]] auto cpp_method_definition(const model &program,
                                         const method_model &method)
    -> std::string {
    code_writer writer;
    writer.blank();
    writer.raw(emit::doc_block(method.documentation, "    /// "));
    writer.line("    {} {{", cpp_method_declaration(program, method));
    for (const auto &parameter : method.parameters) {
        if (parameter.out) {
            writer.line("        ::refrain::contracts::init_struct({});",
                        parameter.name);
            continue;
        }
        // Normalise the mandatory `struct_size` on a local copy for every
        // non-out contract-struct input parameter.
        if (parameter.type.kind == resolved_type::category::named &&
            is_struct(decl_of(program, parameter.type.module_index,
                              parameter.type.decl_index))) {
            writer.line("        const auto abi_{0} = "
                        "::refrain::contracts::with_struct_size({0});",
                        parameter.name);
        }
    }
    std::string arguments{"self_"};
    for (const auto &parameter : method.parameters) {
        arguments += ", ";
        arguments += cpp_arg_expression(program, parameter);
    }
    if (method.can_fail) {
        const auto error{cpp_type(program, method.error_type)};
        if (method.result_type.is_void()) {
            writer.line("        if (const auto code = table_->{}({}); "
                        "code != REFRAIN_STATUS_OK) {{",
                        method.name, arguments);
            writer.line(
                "            return std::unexpected(static_cast<{}>(code));",
                error);
            writer.line("        }}");
            writer.line("        return {{}};");
        } else {
            const auto result_c{c_type(program, method.result_type)};
            writer.line("        ::{} value{{}};", result_c);
            if (method.result_type.kind == resolved_type::category::named &&
                is_struct(decl_of(program, method.result_type.module_index,
                                  method.result_type.decl_index))) {
                writer.line(
                    "        ::refrain::contracts::init_struct(value);");
            }
            writer.line("        if (const auto code = table_->{}({}, &value); "
                        "code != REFRAIN_STATUS_OK) {{",
                        method.name, arguments);
            writer.line(
                "            return std::unexpected(static_cast<{}>(code));",
                error);
            writer.line("        }}");
            writer.line(
                "        return {};",
                cpp_return_conversion(program, method.result_type, "value"));
        }
    } else if (method.result_type.is_void()) {
        writer.line("        table_->{}({});", method.name, arguments);
    } else {
        writer.line("        return {};",
                    cpp_return_conversion(
                        program, method.result_type,
                        std::format("table_->{}({})", method.name, arguments)));
    }
    writer.line("    }}");
    return writer.take();
}

[[nodiscard]] auto callback_parameters_c(const model &program,
                                         const decl_model &decl)
    -> std::string {
    std::string parameters;
    for (const auto &parameter : decl.callback_parameters) {
        if (!parameters.empty()) {
            parameters += ", ";
        }
        parameters += c_type(program, parameter.type) + " " + parameter.name;
    }
    return parameters;
}

[[nodiscard]] auto callback_parameters_cpp(const model &program,
                                           const decl_model &decl)
    -> std::string {
    std::string parameters;
    for (const auto &parameter : decl.callback_parameters) {
        if (!parameters.empty()) {
            parameters += ", ";
        }
        parameters += cpp_type(program, parameter.type) + " " + parameter.name;
    }
    return parameters;
}

[[nodiscard]] auto emit_callback_abi(const model &program,
                                     std::uint32_t module_index,
                                     const decl_model &decl) -> std::string {
    code_writer writer;
    writer.raw(emit::doc_block(decl.documentation, "// "));
    writer.line("typedef {} (*{})({});",
                c_type(program, decl.callback_return_type),
                c_decl_name(program, module_index, decl),
                callback_parameters_c(program, decl));
    writer.blank();
    return writer.take();
}

[[nodiscard]] auto emit_callback_cpp(const model &program,
                                     const decl_model &decl) -> std::string {
    code_writer writer;
    writer.raw(emit::doc_block(decl.documentation, "/// "));
    writer.line("using {} = {} (*)({});", decl.name,
                cpp_type(program, decl.callback_return_type),
                callback_parameters_cpp(program, decl));
    writer.blank();
    return writer.take();
}

[[nodiscard]] auto emit_abi_header(const ridl_context &context,
                                   std::uint32_t module_index) -> std::string {
    const auto &program{context.program};
    const auto &module{program.modules[module_index]};
    const auto path{emit::c_abi_header_path(module.name)};
    code_writer writer;
    writer.raw(emit::banner(context.hash));
    const auto guard{emit::guard_for(path)};
    writer.line("#ifndef {}", guard);
    writer.line("#define {}", guard);
    writer.blank();
    writer.line("#include <refrain/contracts/abi.h>");
    for (const auto dependency : module_dependencies(program, module_index)) {
        writer.line("#include {}",
                    emit::c_abi_include(module_name(program, dependency)));
    }
    writer.blank();
    writer.line("#ifdef __cplusplus");
    writer.line("extern \"C\" {{");
    writer.line("#endif");
    writer.blank();

    if (module_uses_extension_abi(module)) {
        emit_extension_abi_preamble(writer);
    }

    // Forward declarations for every contract struct.
    for (const auto &decl : module.declarations) {
        if (is_struct(decl)) {
            const auto c_name{c_decl_name(program, module_index, decl)};
            writer.line("typedef struct {} {};", c_name, c_name);
        }
    }
    writer.blank();

    for (const auto &decl : module.declarations) {
        if (decl.kind == declaration_kind::enumeration ||
            decl.kind == declaration_kind::flags) {
            writer.raw(emit_enum_abi(program, module_index, decl));
        }
    }
    for (const auto &decl : module.declarations) {
        if (decl.kind != declaration_kind::handle) {
            continue;
        }
        writer.raw(emit::doc_block(decl.documentation, "// "));
        const auto c_name{emit::abi_prefix(module.name) + "_handle_" +
                          decl.name};
        writer.line("typedef uint64_t {};", c_name);
        writer.line("#define {}_INVALID (({})(REFRAIN_HANDLE_INVALID))",
                    emit::upper(c_name), c_name);
        writer.blank();
    }
    for (const auto &decl : module.declarations) {
        if (decl.kind == declaration_kind::callback) {
            writer.raw(emit_callback_abi(program, module_index, decl));
        }
    }
    for (const auto &decl : module.declarations) {
        if (decl.kind != declaration_kind::event) {
            continue;
        }
        // Events are not part of the C ABI: only the stable id is exported, for
        // logging/diagnostics.
        writer.raw(emit::doc_block(decl.documentation, "// "));
        writer.line(
            "#define {}_EVENT_{}_ID \"{}\"",
            emit::upper(emit::abi_prefix(module.name)), emit::upper(decl.name),
            emit::escape_text(event_id_text(program, module_index, decl)));
        writer.blank();
    }
    for (const auto &[key, slice_name] : context.slices) {
        if (key.first != module_index) {
            continue;
        }
        const auto &element_decl{decl_of(program, key.first, key.second)};
        writer.line("typedef struct {} {{", slice_name);
        writer.line("    const {} *data;",
                    c_decl_name(program, key.first, element_decl));
        writer.line("    uint64_t len;");
        writer.line("}} {}; // slice<{}>", slice_name, element_decl.name);
        writer.blank();
    }
    for (const auto decl_index : struct_order(program, module_index)) {
        writer.raw(emit_struct_abi(program, module_index,
                                   module.declarations[decl_index]));
    }

    for (const auto &decl : module.declarations) {
        if (decl.kind == declaration_kind::extension) {
            writer.raw(emit_extension_abi(program, module_index, decl));
        }
    }

    for (const auto &decl : module.declarations) {
        if (decl.kind != declaration_kind::service) {
            continue;
        }
        const auto prefix{service_prefix(program, module_index, decl)};
        const auto upper_prefix{emit::upper(prefix)};
        const auto service_id{service_id_of(program, module_index, decl)};
        writer.raw(emit::doc_block(decl.documentation, "// "));
        writer.line("#define {}_SERVICE_ID \"{}\"", upper_prefix,
                    emit::escape_text(service_id));
        writer.line("#define {}_ABI_MAJOR {}u", upper_prefix,
                    decl.service_version_major);
        writer.line("#define {}_ABI_MINOR {}u", upper_prefix,
                    decl.service_version_minor);
        writer.line("#define {}_ABI_PATCH {}u", upper_prefix,
                    decl.service_version_patch);
        writer.line("#define {}_SIDE {}", upper_prefix,
                    c_side_macro(decl.service.side));
        writer.line("#define {}_THREAD {}", upper_prefix,
                    c_thread_macro(decl.service.thread));
        // Contract-level composition mode: decides how many provider
        // instances may be active for this service at runtime.
        writer.line("#define {}_COMPOSITION {}", upper_prefix,
                    c_composition_macro(decl.service.composition));
        writer.line("#define {}_CAPABILITIES \"{}\"", upper_prefix,
                    emit::escape_text(join(decl.service.capabilities, ",")));
        writer.line("#define {}_REQUIRES \"{}\"", upper_prefix,
                    emit::escape_text(join(decl.service.requires_list, ",")));
        // Every `provides` entry, not just the first (which stays
        // `_SERVICE_ID` for backwards compatibility).
        writer.line("#define {}_SERVICE_IDS \"{}\"", upper_prefix,
                    emit::escape_text(join(decl.service.provides, ",")));
        if (!decl.extension_points.empty()) {
            emit_service_extension_abi(writer, program, module_index, decl);
        }
        writer.blank();
        writer.line("typedef struct {}_vtable {{", prefix);
        writer.line("    uint32_t struct_size;");
        writer.line("    uint32_t abi_major;");
        writer.line("    uint32_t abi_minor;");
        // Layout is unchanged from the original `reserved_0` slot; the field
        // now carries the contract patch version (SemVer major.minor.patch).
        writer.line("    uint32_t abi_patch;");
        for (const auto &method : decl.methods) {
            writer.raw(emit::doc_block(method.documentation, "    // "));
            writer.raw(emit::attribute_comment(method.attributes, "    "));
            writer.line("    {};", vtable_signature(program, method));
        }
        emit_service_query_slot(writer, decl);
        writer.line("}} {}_vtable; // service {}", prefix,
                    emit::escape_text(service_id));
        writer.blank();
        writer.line("#define {}_VTABLE_SIZE ((uint32_t)sizeof({}_vtable))",
                    upper_prefix, prefix);
        writer.blank();
    }

    writer.line("#ifdef __cplusplus");
    writer.line("}} // extern \"C\"");
    writer.line("#endif");
    writer.blank();
    writer.line("#endif // {}", guard);
    return writer.take();
}

/// Scheduled methods in declaration order (descriptor/trampoline order).
[[nodiscard]] auto scheduled_methods(const decl_model &decl)
    -> std::vector<const method_model *> {
    std::vector<const method_model *> methods;
    for (const auto &method : decl.methods) {
        if (method.scheduled) {
            methods.push_back(&method);
        }
    }
    return methods;
}

/// `#[schedule]` trampolines + `schedule_tasks` for one service.
/// Emitted inside the service namespace after the
/// `provider` wrapper, because each trampoline constructs it. The empty table
/// is still emitted so hosts can uniformly take `std::span{schedule_tasks}`.
[[nodiscard]] auto emit_schedule_table(const model &program,
                                       std::uint32_t module_index,
                                       const decl_model &decl) -> std::string {
    const auto prefix{service_prefix(program, module_index, decl)};
    const auto service{service_id_of(program, module_index, decl)};
    const auto provider_type{
        "::" + emit::cpp_namespace(module_name(program, module_index)) +
        "::provider"};
    const auto methods{scheduled_methods(decl)};
    code_writer writer;
    const auto put = [&writer](std::string text) { writer.line("{}", text); };
    if (!methods.empty()) {
        put("namespace schedule_detail {");
        for (std::size_t index = 0; index < methods.size(); ++index) {
            const auto &method{*methods[index]};
            put(std::format("inline auto invoke_{}(void *self, const void "
                            "*vtable, std::int64_t tick) noexcept",
                            index));
            put("    -> ::refrain::status {");
            // The trampoline signature is fixed; the tick is not forwarded yet
            // (reserved for tick-aware hooks).
            put("    static_cast<void>(tick);");
            put(std::format("    const auto provider{{{}{{", provider_type));
            // Plain concatenation: the two closing braces must survive
            // verbatim (std::format would read `}}` as an escape).
            put("        *static_cast<const ::" + prefix +
                "_vtable *>(vtable), self}};");
            if (method.can_fail) {
                put(std::format("    if (auto result{{provider.{}()}}; "
                                "!result) {{",
                                method.name));
                put("        return "
                    "std::unexpected{::refrain::make_error(");
                put("            ::refrain::error_code::unknown,");
                put(std::format("            std::string{{\"{}.{} "
                                "failed\"}})}};",
                                service, method.name));
                put("    }");
            } else {
                // Plain `void` method: the provider call cannot fail, so the
                // trampoline only needs the call (nodiscard-safe cast).
                put(std::format("    static_cast<void>(provider.{}());",
                                method.name));
            }
            put("    return {};");
            put("}");
        }
        put("} // namespace schedule_detail");
        put("");
    }
    put(std::format("inline constexpr "
                    "std::array<::refrain::core::schedule::task_descriptor, "
                    "{}>",
                    methods.size()));
    if (methods.empty()) {
        put("    schedule_tasks{};");
    } else {
        put("    schedule_tasks{{");
        for (std::size_t index = 0; index < methods.size(); ++index) {
            const auto &method{*methods[index]};
            put("        "
                "::refrain::core::schedule::task_descriptor{");
            put("            .service = service_id,");
            put(std::format("            .method = \"{}\",", method.name));
            put(std::format("            .interval_ticks = {},",
                            method.schedule_interval_ticks));
            put(std::format("            .phase_of = "
                            "::refrain::core::schedule::phase::{},",
                            method.schedule_phase_of ==
                                    schedule_phase_kind::pre_tick
                                ? "pre_tick"
                                : "post_tick"));
            put(std::format("            .invoke = "
                            "&schedule_detail::invoke_{},",
                            index));
            put("        },");
        }
        put("    }};");
    }
    put("");
    return writer.take();
}

[[nodiscard]] auto emit_cpp_header(const ridl_context &context,
                                   std::uint32_t module_index) -> std::string {
    const auto &program{context.program};
    const auto &module{program.modules[module_index]};
    const auto path{emit::cpp_header_path(module.name)};
    code_writer writer;
    writer.raw(emit::banner(context.hash));
    const auto guard{emit::guard_for(path)};
    writer.line("#ifndef {}", guard);
    writer.line("#define {}", guard);
    writer.blank();
    writer.line("#include {}", emit::c_abi_include(module.name));
    writer.line("#include <refrain/contracts/abi.hpp>");
    writer.line("#include <array>");
    writer.line("#include <cstddef>");
    writer.line("#include <cstdint>");
    writer.line("#include <expected>");
    writer.line("#include <span>");
    writer.line("#include <string>");
    writer.line("#include <string_view>");
    for (const auto dependency : module_dependencies(program, module_index)) {
        writer.line("#include {}",
                    emit::cpp_include(module_name(program, dependency)));
    }
    const auto has_events{
        std::ranges::any_of(module.declarations, [](const decl_model &decl) {
            return decl.kind == declaration_kind::event;
        })};
    if (has_events) {
        writer.line("#include <refrain/contracts/event.hpp>");
    }
    writer.line("#include <refrain/core/error.hpp>");
    writer.line("#include <refrain/core/schedule.hpp>");
    writer.blank();
    writer.line("namespace {} {{", emit::cpp_namespace(module.name));
    writer.blank();

    for (const auto &decl : module.declarations) {
        if (decl.kind == declaration_kind::enumeration ||
            decl.kind == declaration_kind::flags) {
            writer.raw(emit_enum_cpp(decl));
        }
    }

    // Opaque handle tags + aliases.
    bool has_handles{false};
    for (const auto &decl : module.declarations) {
        if (decl.kind == declaration_kind::handle) {
            if (!has_handles) {
                writer.line("namespace detail {{");
                has_handles = true;
            }
            writer.line("struct {}_tag;", decl.name);
        }
    }
    if (has_handles) {
        writer.line("}} // namespace detail");
        writer.blank();
        for (const auto &decl : module.declarations) {
            if (decl.kind != declaration_kind::handle) {
                continue;
            }
            writer.raw(emit::doc_block(decl.documentation, "/// "));
            writer.line(
                "using {} = ::refrain::contracts::handle<detail::{}_tag>;",
                decl.name, decl.name);
        }
        writer.blank();
    }

    for (const auto &decl : module.declarations) {
        if (decl.kind == declaration_kind::callback) {
            writer.raw(emit_callback_cpp(program, decl));
        }
    }

    // Events: after the handle aliases, before the contract structs.
    // Fields are in declaration order with explicit
    // padding; no `struct_size` because events are not ABI structs.
    for (const auto &decl : module.declarations) {
        if (decl.kind == declaration_kind::event) {
            writer.raw(emit_event_cpp(program, module_index, decl));
        }
    }

    // Contract struct aliases + ABI layout assertions.
    for (const auto &decl : module.declarations) {
        if (!is_struct(decl)) {
            continue;
        }
        writer.raw(emit::doc_block(decl.documentation, "/// "));
        writer.line("using {} = ::{};", decl.name,
                    c_decl_name(program, module_index, decl));
        // Explicit construction helper so callers never forget the
        // mandatory `struct_size` field (the wrappers also fix it up, but the
        // helper makes the intended pattern obvious).
        writer.line("/// Zero-initialized `{0}` with the mandatory "
                    "`struct_size` set; fill the fields before passing it "
                    "across the ABI.",
                    decl.name);
        writer.line("[[nodiscard]] inline auto make_{0}() noexcept -> {0} {{",
                    decl.name);
        writer.line("    {0} value{{}};", decl.name);
        writer.line("    ::refrain::contracts::init_struct(value);");
        writer.line("    return value;");
        writer.line("}}");
    }
    writer.blank();
    for (const auto &decl : module.declarations) {
        if (!is_struct(decl)) {
            continue;
        }
        writer.line("static_assert(sizeof({}) == {});", decl.name,
                    decl.abi_size);
        writer.line("static_assert(alignof({}) == {});", decl.name,
                    decl.abi_alignment);
        writer.line("static_assert(offsetof({}, struct_size) == 0);",
                    decl.name);
        for (const auto &field : decl.fields) {
            writer.line("static_assert(offsetof({}, {}) == {});", decl.name,
                        field.name, field.abi_offset);
        }
    }
    writer.blank();

    for (const auto &decl : module.declarations) {
        if (decl.kind == declaration_kind::extension) {
            writer.raw(emit_extension_cpp(program, module_index, decl));
        }
    }

    for (const auto &decl : module.declarations) {
        if (decl.kind != declaration_kind::service) {
            continue;
        }
        const auto prefix{service_prefix(program, module_index, decl)};
        const auto service_id{service_id_of(program, module_index, decl)};
        writer.raw(emit::doc_block(decl.documentation, "/// "));
        writer.line("inline constexpr std::string_view service_id = \"{}\";",
                    emit::escape_text(service_id));
        writer.line("inline constexpr std::uint32_t abi_major = {};",
                    decl.service_version_major);
        writer.line("inline constexpr std::uint32_t abi_minor = {};",
                    decl.service_version_minor);
        writer.line("inline constexpr std::uint32_t abi_patch = {};",
                    decl.service_version_patch);
        writer.line("inline constexpr ::refrain::contracts::side_kind side = "
                    "::refrain::contracts::side_kind::{};",
                    cpp_side_name(decl.service.side));
        writer.line("inline constexpr ::refrain::contracts::thread_kind thread "
                    "= ::refrain::contracts::thread_kind::{};",
                    cpp_thread_name(decl.service.thread));
        writer.line("inline constexpr ::refrain::contracts::composition_kind "
                    "composition = "
                    "::refrain::contracts::composition_kind::{};",
                    cpp_composition_name(decl.service.composition));
        writer.line("inline constexpr std::string_view capabilities = \"{}\";",
                    emit::escape_text(join(decl.service.capabilities, ",")));
        writer.line(
            "inline constexpr std::string_view required_services = \"{}\";",
            emit::escape_text(join(decl.service.requires_list, ",")));
        // All `provides` entries are exposed (service_id keeps the first).
        writer.raw("inline constexpr std::string_view service_ids[] = {");
        for (std::size_t index = 0; index < decl.service.provides.size();
             ++index) {
            if (index != 0) {
                writer.raw(", ");
            }
            writer.raw("\"");
            writer.raw(emit::escape_text(decl.service.provides[index]));
            writer.raw("\"");
        }
        writer.line("}};");
        if (!decl.extension_points.empty()) {
            writer.line("inline constexpr std::string_view extension_ids[] = "
                        "{{");
            for (std::size_t index = 0; index < decl.extension_points.size();
                 ++index) {
                if (index != 0) {
                    writer.raw(", ");
                }
                writer.raw("\"");
                writer.raw(emit::escape_text(
                    decl_of(program, decl.extension_points[index].module_index,
                            decl.extension_points[index].decl_index)
                        .extension_id));
                writer.raw("\"");
            }
            writer.line("}};");
        }
        writer.blank();

        if (!decl.extension_points.empty()) {
            writer.raw(emit_extension_support_cpp());
            writer.blank();
        }

        writer.line("/// Type-safe wrapper around `{}_vtable`. The provider "
                    "(host) owns the table and `self`; this class is a "
                    "borrowed call surface.",
                    prefix);
        writer.line("class provider final {{");
        writer.line("  public:");
        writer.line("    provider(const ::{}_vtable &table, void *self) "
                    "noexcept",
                    prefix);
        writer.line("        : table_{{&table}}, self_{{self}} {{}}");
        writer.blank();
        writer.line("    provider(const provider &) = delete;");
        writer.line(
            "    auto operator=(const provider &) -> provider & = delete;");
        writer.blank();
        writer.line("    [[nodiscard]] auto table() const noexcept");
        writer.line("        -> const ::{}_vtable & {{", prefix);
        writer.line("        return *table_;");
        writer.line("    }}");
        writer.blank();
        writer.line("    [[nodiscard]] auto self() const noexcept -> void * "
                    "{{");
        writer.line("        return self_;");
        writer.line("    }}");
        for (const auto &method : decl.methods) {
            writer.raw(cpp_method_definition(program, method));
        }
        if (!decl.extension_points.empty()) {
            writer.line("    using service_vtable = ::{}_vtable;", prefix);
            writer.raw(emit_provider_extension_api());
        }
        writer.blank();
        writer.line("  private:");
        writer.line("    const ::{}_vtable *table_;", prefix);
        writer.line("    void *self_;");
        writer.line("}};");
        writer.blank();
        writer.raw(emit_schedule_table(program, module_index, decl));
    }

    writer.line("}} // namespace {}", emit::cpp_namespace(module.name));
    writer.blank();
    writer.raw(emit_event_traits(program, module_index));
    writer.line("#endif // {}", guard);
    return writer.take();
}

} // namespace

auto emit_ridl(const model &program, std::string_view schema_hash)
    -> std::vector<emit::generated_file> {
    ridl_context context{program, schema_hash, {}};
    for (const auto &module : program.modules) {
        if (module.kind != dialect::ridl) {
            continue;
        }
        for (const auto &decl : module.declarations) {
            // Event slices are C++-only (`std::span<const scalar>`); they have
            // no C ABI slice struct and must not be collected as one.
            if (decl.kind == declaration_kind::event) {
                continue;
            }
            for (const auto &field : decl.fields) {
                collect_slice_types(program, field.type, context);
            }
            for (const auto &method : decl.methods) {
                for (const auto &parameter : method.parameters) {
                    collect_slice_types(program, parameter.type, context);
                }
                if (method.can_fail) {
                    collect_slice_types(program, method.error_type, context);
                }
                collect_slice_types(program, method.result_type, context);
            }
        }
    }

    std::vector<emit::generated_file> files;
    std::vector<std::string> c_includes;
    std::vector<std::string> cpp_includes;
    for (std::uint32_t module_index = 0; module_index < program.modules.size();
         ++module_index) {
        if (program.modules[module_index].kind != dialect::ridl) {
            continue;
        }
        files.push_back(emit::generated_file{
            emit::c_abi_header_path(program.modules[module_index].name),
            emit_abi_header(context, module_index)});
        files.push_back(emit::generated_file{
            emit::cpp_header_path(program.modules[module_index].name),
            emit_cpp_header(context, module_index)});
        c_includes.push_back(
            emit::c_abi_include(program.modules[module_index].name));
        cpp_includes.push_back(
            emit::cpp_include(program.modules[module_index].name));
    }

    if (!c_includes.empty()) {
        std::sort(c_includes.begin(), c_includes.end());
        std::sort(cpp_includes.begin(), cpp_includes.end());
        code_writer c_aggregate;
        c_aggregate.raw(emit::banner(context.hash));
        c_aggregate.line("#ifndef REFRAIN_GENERATED_C_REFRAIN_CONTRACTS_H");
        c_aggregate.line("#define REFRAIN_GENERATED_C_REFRAIN_CONTRACTS_H");
        c_aggregate.blank();
        for (const auto &include : c_includes) {
            c_aggregate.line("#include {}", include);
        }
        c_aggregate.blank();
        c_aggregate.line("#endif // REFRAIN_GENERATED_C_REFRAIN_CONTRACTS_H");
        files.push_back(
            emit::generated_file{"c/refrain/contracts.h", c_aggregate.take()});

        code_writer cpp_aggregate;
        cpp_aggregate.raw(emit::banner(context.hash));
        cpp_aggregate.line(
            "#ifndef REFRAIN_GENERATED_CPP_REFRAIN_CONTRACTS_HPP");
        cpp_aggregate.line(
            "#define REFRAIN_GENERATED_CPP_REFRAIN_CONTRACTS_HPP");
        cpp_aggregate.blank();
        for (const auto &include : cpp_includes) {
            cpp_aggregate.line("#include {}", include);
        }
        cpp_aggregate.blank();
        cpp_aggregate.line(
            "#endif // REFRAIN_GENERATED_CPP_REFRAIN_CONTRACTS_HPP");
        files.push_back(emit::generated_file{emit::cpp_aggregate_header_path(),
                                             cpp_aggregate.take()});
    }

    std::sort(
        files.begin(), files.end(),
        [](const emit::generated_file &lhs, const emit::generated_file &rhs) {
            return lhs.path < rhs.path;
        });
    return files;
}

auto emit_reflection(const model &program, std::string_view schema_hash)
    -> emit::generated_file {
    const auto path{emit::cpp_reflection_header_path()};
    const auto guard{emit::guard_for(path)};
    code_writer writer;
    writer.raw(emit::banner(schema_hash));
    writer.line("#ifndef {}", guard);
    writer.line("#define {}", guard);
    writer.blank();
    // The whole body is conditional: with reflection disabled this header is a
    // no-op, so ordinary consumers of the generated model never pull refl-cpp
    // in. Nothing else in the generated output set includes reflect.hpp.
    writer.line(
        "#if defined(REFRAIN_USE_REFLECTION) && REFRAIN_USE_REFLECTION");
    writer.blank();
    writer.raw(
        "// Optional reflection metadata over the generated C++ model: one\n"
        "// custom_traits<T> specialization per generated struct/record (RSC\n"
        "// + RIDL) with the qualified type name and the schema fields in\n"
        "// declaration order. ABI-only members (`struct_size`, `reserved_N`\n"
        "// padding) are not schema fields, so they are not reflected. With\n"
        "// REFRAIN_USE_REFLECTION disabled this header is empty; normal\n"
        "// consumers of the generated model never depend on refl-cpp.\n");
    writer.blank();

    // A declaration is only described when its generated header exists: RIDL
    // records live in the aggregate `contracts.hpp`, RSC records in the header
    // generated from `decl.source_path`.
    const auto in_output_set = [](const module_model &module,
                                  const decl_model &decl) {
        return is_reflectable_decl(decl) &&
               (module.kind == dialect::ridl || !decl.source_path.empty());
    };

    const auto is_extension_decl = [](const decl_model &decl) {
        return decl.kind == declaration_kind::extension;
    };

    bool has_records{false};
    bool has_extensions{false};
    std::set<std::string> rsc_includes;
    for (std::uint32_t module_index = 0; module_index < program.modules.size();
         ++module_index) {
        const auto &module{program.modules[module_index]};
        for (const auto &decl : module.declarations) {
            if (is_extension_decl(decl)) {
                has_extensions = true;
                has_records = true; // extensions live in the aggregate header
                continue;
            }
            if (!in_output_set(module, decl)) {
                continue;
            }
            if (module.kind == dialect::rsc) {
                rsc_includes.insert(emit::rsc_cpp_include(decl.source_path));
            } else {
                has_records = true;
            }
        }
    }
    if (has_records) {
        writer.line("#include {}", emit::cpp_aggregate_include());
    }
    for (const auto &include : rsc_includes) {
        writer.line("#include {}", include);
    }
    writer.blank();
    writer.line("#include <cstdint>");
    writer.line("#include <string_view>");
    writer.line("#include <tuple>");
    writer.blank();
    writer.line("#include <refrain/reflect.hpp>");
    writer.blank();

    const auto any_metadata{has_records || has_extensions};
    if (any_metadata) {
        writer.line("namespace refrain::reflect {{");
        writer.blank();
    }
    if (has_extensions) {
        writer.raw(
            "/// Extension metadata specialization point for generated RIDL\n"
            "/// extensions. The primary template is empty; the generator\n"
            "/// specializes it per declared extension.\n");
        writer.line(
            "template <typename T> struct custom_extension_traits {{}};");
        writer.blank();
    }

    for (std::uint32_t module_index = 0; module_index < program.modules.size();
         ++module_index) {
        const auto &module{program.modules[module_index]};
        for (const auto &decl : module.declarations) {
            if (!in_output_set(module, decl)) {
                continue;
            }
            const auto type{cpp_type_name(program, module_index, decl)};
            writer.line("/// Generated {} {} '{}'.", dialect_name(module.kind),
                        declaration_kind_name(decl.kind), type);
            writer.line("template <> struct custom_traits<::{}> {{", type);
            writer.line("    static constexpr std::string_view type_name = "
                        "\"{}\";",
                        emit::escape_text(type));
            if (decl.fields.empty()) {
                writer.line(
                    "    static constexpr auto fields = std::make_tuple();");
            } else {
                writer.line(
                    "    static constexpr auto fields = std::make_tuple(");
                for (std::size_t index = 0; index < decl.fields.size();
                     ++index) {
                    const auto &field{decl.fields[index]};
                    writer.line(
                        "        refrain::reflect::custom_field{{\"{}\", "
                        "&::{}::{}}}{}",
                        emit::escape_text(field.name), type, field.name,
                        index + 1 == decl.fields.size() ? ");" : ",");
                }
            }
            writer.line("}};");
            writer.blank();
        }
    }

    for (std::uint32_t module_index = 0; module_index < program.modules.size();
         ++module_index) {
        const auto &module{program.modules[module_index]};
        for (const auto &decl : module.declarations) {
            if (!is_extension_decl(decl)) {
                continue;
            }
            const auto type{cpp_type_name(program, module_index, decl)};
            writer.line("/// Generated RIDL extension '{}'.",
                        emit::escape_text(decl.extension_id));
            writer.line("template <> struct custom_extension_traits<::{}> {{",
                        type);
            writer.line("    static constexpr std::string_view id = "
                        "\"{}\";",
                        emit::escape_text(decl.extension_id));
            writer.line("    static constexpr std::string_view extends = "
                        "\"{}\";",
                        emit::escape_text(decl.extension.extends_service));
            writer.line("    static constexpr std::string_view capability = "
                        "\"{}\";",
                        emit::escape_text(decl.extension.capability));
            writer.line("    static constexpr std::uint32_t abi_major = {};",
                        decl.extension_version_major);
            writer.line("    static constexpr std::uint32_t abi_minor = {};",
                        decl.extension_version_minor);
            writer.line("    static constexpr std::uint32_t abi_patch = {};",
                        decl.extension_version_patch);
            writer.line("}};");
            writer.blank();
        }
    }

    if (any_metadata) {
        writer.line("}} // namespace refrain::reflect");
    } else {
        writer.line("// No generated struct/record declarations to describe.");
    }
    writer.blank();
    writer.line("#endif // REFRAIN_USE_REFLECTION");
    writer.blank();
    writer.line("#endif // {}", guard);
    return emit::generated_file{path, writer.take()};
}

auto emit_metadata(const model &program, std::string_view schema_hash)
    -> emit::generated_file {
    const auto path{emit::cpp_metadata_header_path()};
    const auto guard{emit::guard_for(path)};
    code_writer writer;
    writer.raw(emit::banner(schema_hash));
    writer.line("#ifndef {}", guard);
    writer.line("#define {}", guard);
    writer.blank();
    writer.line("#include <array>");
    writer.line("#include <cstddef>");
    writer.line("#include <cstdint>");
    writer.line("#include <span>");
    writer.line("#include <string_view>");
    writer.blank();
    writer.line("// Generated runtime schema metadata. This is data for the");
    writer.line(
        "// sealed `refrain:host.metadata` service, not C++ `reflect<T>`.");
    writer.line("namespace refrain::metadata {{");
    writer.blank();
    writer.line("struct enum_value_entry {{");
    writer.line("    std::string_view name;");
    writer.line("    std::int64_t value;");
    writer.line("}};");
    writer.blank();
    writer.line("struct field_entry {{");
    writer.line("    std::string_view name;");
    writer.line("    std::string_view type_id;");
    writer.line("    std::uint32_t offset;");
    writer.line("    std::uint32_t size;");
    writer.line("    std::uint32_t flags;");
    writer.line("}};");
    writer.blank();
    writer.line("struct type_entry {{");
    writer.line("    std::string_view id;");
    writer.line("    std::uint32_t kind;");
    writer.line("    std::uint32_t size;");
    writer.line("    std::uint32_t alignment;");
    writer.line("    std::span<const field_entry> fields;");
    writer.line("    std::span<const enum_value_entry> values;");
    writer.line("}};");
    writer.blank();
    writer.line("struct contract_entry {{");
    writer.line("    std::string_view id;");
    writer.line("    std::uint32_t abi_major;");
    writer.line("    std::uint32_t abi_minor;");
    writer.line("    std::uint32_t abi_patch;");
    writer.line("    std::string_view schema_hash;");
    writer.line("    std::uint32_t method_count;");
    writer.line("}};");
    writer.blank();

    const auto type_id = [&](const auto &self,
                             const resolved_type &type) -> std::string {
        switch (type.kind) {
        case resolved_type::category::primitive:
            return std::string{primitive_name(type.primitive)};
        case resolved_type::category::named:
            return cpp_type_name(
                program, type.module_index,
                decl_of(program, type.module_index, type.decl_index));
        case resolved_type::category::handle:
            return "handle:" +
                   decl_of(program, type.module_index, type.decl_index).name;
        case resolved_type::category::slice:
            return "slice<" + self(self, type.arguments.front()) + ">";
        case resolved_type::category::array:
            return "array<" + self(self, type.arguments.front()) + ">";
        case resolved_type::category::optional:
            return "optional<" + self(self, type.arguments.front()) + ">";
        case resolved_type::category::repeated:
            return "repeated<" + self(self, type.arguments.front()) + ">";
        }
        return "?";
    };
    const auto type_id_of = [&](const resolved_type &type) {
        return type_id(type_id, type);
    };
    const auto integer_size = [](std::string_view name) -> std::uint32_t {
        if (name == "u8" || name == "i8" || name == "bool") {
            return 1;
        }
        if (name == "u16" || name == "i16") {
            return 2;
        }
        if (name == "u32" || name == "i32" || name == "f32") {
            return 4;
        }
        if (name == "u64" || name == "i64" || name == "f64") {
            return 8;
        }
        return 0;
    };

    writer.line("namespace detail {{");
    std::vector<std::string> detail_arrays{};
    for (const auto &module : program.modules) {
        if (module.kind != dialect::ridl) {
            continue;
        }
        for (const auto &decl : module.declarations) {
            const auto prefix{emit::abi_prefix(module.name) + "_" + decl.name};
            if (decl.kind == declaration_kind::structure) {
                writer.line("inline constexpr std::array<field_entry, {}> "
                            "{}_fields{{",
                            decl.fields.size(), prefix);
                for (const auto &field : decl.fields) {
                    writer.line(
                        "    field_entry{{\"{}\" , \"{}\", {}, {}, 0}},",
                        emit::escape_text(field.name),
                        emit::escape_text(type_id_of(field.type)),
                        field.abi_offset, field.abi_size);
                }
                writer.line("}};");
                detail_arrays.push_back(prefix + "_fields");
            } else if (decl.kind == declaration_kind::enumeration ||
                       decl.kind == declaration_kind::flags) {
                writer.line("inline constexpr std::array<enum_value_entry, {}> "
                            "{}_values{{",
                            decl.entries.size(), prefix);
                for (const auto &entry : decl.entries) {
                    writer.line("    enum_value_entry{{\"{}\" , {}}},",
                                emit::escape_text(entry.name),
                                entry.value.value_or(0));
                }
                writer.line("}};");
                detail_arrays.push_back(prefix + "_values");
            }
        }
    }
    writer.line("}} // namespace detail");
    writer.blank();

    std::vector<std::string> type_entries{};
    for (const auto &module : program.modules) {
        if (module.kind != dialect::ridl) {
            continue;
        }
        for (const auto &decl : module.declarations) {
            if (decl.kind != declaration_kind::structure &&
                decl.kind != declaration_kind::enumeration &&
                decl.kind != declaration_kind::flags) {
                continue;
            }
            const auto id{cpp_type_name(
                program,
                static_cast<std::uint32_t>(&module - program.modules.data()),
                decl)};
            const auto kind{
                decl.kind == declaration_kind::structure
                    ? 0U
                    : (decl.kind == declaration_kind::enumeration ? 1U : 2U)};
            const auto size{decl.kind == declaration_kind::structure
                                ? decl.abi_size
                                : integer_size(decl.underlying)};
            const auto alignment{decl.kind == declaration_kind::structure
                                     ? decl.abi_alignment
                                     : integer_size(decl.underlying)};
            const auto prefix{emit::abi_prefix(module.name) + "_" + decl.name};
            std::string fields{
                "std::span<const field_entry>{detail::" + prefix + "_fields}"};
            std::string values{"std::span<const enum_value_entry>{detail::" +
                               prefix + "_values}"};
            if (decl.kind != declaration_kind::structure) {
                fields = "std::span<const field_entry>{}";
            } else {
                values = "std::span<const enum_value_entry>{}";
            }
            type_entries.push_back(std::format(
                "    type_entry{{\"{}\", {}, {}, {}, {}, {}}},",
                emit::escape_text(id), kind, size, alignment, fields, values));
        }
    }
    writer.line("inline constexpr std::array<type_entry, {}> types{{",
                type_entries.size());
    for (const auto &entry : type_entries) {
        writer.line("{}", entry);
    }
    writer.line("}};");
    writer.blank();

    std::vector<std::string> contract_entries{};
    for (const auto &module : program.modules) {
        if (module.kind != dialect::ridl) {
            continue;
        }
        for (const auto &decl : module.declarations) {
            if (decl.kind != declaration_kind::service) {
                continue;
            }
            const auto id{service_id_of(
                program,
                static_cast<std::uint32_t>(&module - program.modules.data()),
                decl)};
            contract_entries.push_back(std::format(
                "    contract_entry{{\"{}\", {}, {}, {}, \"{}\", {}}},",
                emit::escape_text(id), decl.service_version_major,
                decl.service_version_minor, decl.service_version_patch,
                emit::escape_text(schema_hash), decl.methods.size()));
        }
    }
    writer.line("inline constexpr std::array<contract_entry, {}> contracts{{",
                contract_entries.size());
    for (const auto &entry : contract_entries) {
        writer.line("{}", entry);
    }
    writer.line("}};");
    writer.blank();
    writer.line("inline constexpr std::string_view schema_hash = \"{}\";",
                emit::escape_text(schema_hash));
    writer.blank();
    writer.line("}} // namespace refrain::metadata");
    writer.blank();
    writer.line("#endif // {}", guard);
    return emit::generated_file{path, writer.take()};
}

auto emit_rust(const model &program, std::string_view schema_hash)
    -> emit::generated_file {
    const auto path{emit::rust_bindings_path()};
    code_writer writer;
    writer.raw(emit::banner(schema_hash));
    writer.line("// Rust bindings for the sealed Refrain host contracts.");
    writer.line("// C ABI layout is authoritative; this file mirrors it.");
    writer.line("#![allow(non_camel_case_types, non_snake_case, dead_code)]");
    writer.blank();
    writer.line("#[repr(C)]");
    writer.line("pub struct RefrainStr {{");
    writer.line("    pub data: *const u8,");
    writer.line("    pub len: u64,");
    writer.line("}}");
    writer.blank();
    writer.line("#[repr(C)]");
    writer.line("pub struct RefrainBytes {{");
    writer.line("    pub data: *const u8,");
    writer.line("    pub len: u64,");
    writer.line("}}");
    writer.blank();
    writer.line("#[repr(C)]");
    writer.line("pub struct RefrainMutBytes {{");
    writer.line("    pub data: *mut u8,");
    writer.line("    pub len: u64,");
    writer.line("}}");
    writer.blank();
    writer.line("#[repr(C)]");
    writer.line("pub struct RefrainSlice<T> {{");
    writer.line("    pub data: *const T,");
    writer.line("    pub len: u64,");
    writer.line("}}");
    writer.blank();
    writer.line("pub type RefrainStatus = u32;");
    writer.line("pub const REFRAIN_STATUS_OK: RefrainStatus = 0;");
    writer.line("pub const REFRAIN_HANDLE_INVALID: u64 = 0;");
    writer.blank();

    const auto rust_type = [&](const auto &self,
                               const resolved_type &type) -> std::string {
        switch (type.kind) {
        case resolved_type::category::primitive:
            switch (type.primitive) {
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
                return "u8";
            case primitive_kind::string:
                return "RefrainStr";
            case primitive_kind::bytes:
                return "RefrainBytes";
            case primitive_kind::mut_bytes:
                return "RefrainMutBytes";
            case primitive_kind::opaque_ptr:
                return "*mut ::core::ffi::c_void";
            case primitive_kind::void_type:
                return "()";
            }
            return "()";
        case resolved_type::category::named:
            return decl_of(program, type.module_index, type.decl_index).name;
        case resolved_type::category::handle:
            return decl_of(program, type.module_index, type.decl_index).name;
        case resolved_type::category::slice:
            return "RefrainSlice<" + self(self, type.arguments.front()) + ">";
        case resolved_type::category::array:
        case resolved_type::category::optional:
        case resolved_type::category::repeated:
            return "()";
        }
        return "()";
    };
    const auto rust_type_of = [&](const resolved_type &type) {
        return rust_type(rust_type, type);
    };
    const auto rust_field = [](std::string_view name) -> std::string {
        if (name == "self") {
            return "self_";
        }
        static const std::set<std::string_view> keywords{
            "as",       "break",  "const", "continue", "crate",  "else",
            "enum",     "extern", "false", "fn",       "for",    "if",
            "impl",     "in",     "let",   "loop",     "match",  "mod",
            "move",     "mut",    "pub",   "ref",      "return", "static",
            "struct",   "super",  "trait", "true",     "type",   "unsafe",
            "use",      "where",  "while", "async",    "await",  "dyn",
            "abstract", "become", "box",   "do",       "final",  "macro",
            "override", "priv",   "try",   "typeof",   "union",  "unsized",
            "virtual",  "yield",
        };
        if (keywords.contains(name)) {
            return "r#" + std::string{name};
        }
        return std::string{name};
    };
    const auto rust_method = [&](std::string_view name) {
        return rust_field(name);
    };

    for (const auto &module : program.modules) {
        if (module.kind != dialect::ridl ||
            !module.name.starts_with("refrain::host")) {
            continue;
        }
        const auto module_index{
            static_cast<std::uint32_t>(&module - program.modules.data())};
        writer.line("pub mod {} {{", module_last_segment(module.name));
        writer.line("    use super::*;");
        writer.blank();

        for (const auto &decl : module.declarations) {
            if (decl.kind == declaration_kind::enumeration ||
                decl.kind == declaration_kind::flags) {
                writer.line("    pub type {} = u32;", decl.name);
                for (const auto &entry : decl.entries) {
                    writer.line("    pub const {}_{}: {} = {};",
                                emit::upper(decl.name), emit::upper(entry.name),
                                decl.name, entry.value.value_or(0));
                }
                writer.blank();
            }
        }
        for (const auto &decl : module.declarations) {
            if (decl.kind == declaration_kind::handle) {
                writer.line("    pub type {} = u64;", decl.name);
                writer.line("    pub const {}_INVALID: {} = 0;",
                            emit::upper(decl.name), decl.name);
                writer.blank();
            }
        }
        for (const auto &decl : module.declarations) {
            if (decl.kind != declaration_kind::callback) {
                continue;
            }
            std::string parameters;
            for (const auto &parameter : decl.callback_parameters) {
                if (!parameters.empty()) {
                    parameters += ", ";
                }
                parameters += rust_field(parameter.name) + ": " +
                              rust_type_of(parameter.type);
            }
            writer.line("    pub type {} = Option<unsafe extern \"C\" fn({})>;",
                        decl.name, parameters);
            writer.blank();
        }
        for (const auto &decl : module.declarations) {
            if (decl.kind != declaration_kind::structure) {
                continue;
            }
            writer.line("    #[repr(C)]");
            writer.line("    pub struct {} {{", decl.name);
            writer.line("        pub struct_size: u32,");
            for (const auto &field : decl.fields) {
                writer.line("        pub {}: {},", rust_field(field.name),
                            rust_type_of(field.type));
            }
            writer.line("    }}");
            writer.blank();
        }
        for (const auto &decl : module.declarations) {
            if (decl.kind != declaration_kind::structure) {
                continue;
            }
            writer.line(
                "    const _: () = assert!(::core::mem::size_of::<{}>() == "
                "{});",
                decl.name, decl.abi_size);
            writer.line(
                "    const _: () = assert!(::core::mem::align_of::<{}>() == "
                "{});",
                decl.name, decl.abi_alignment);
            for (const auto &field : decl.fields) {
                writer.line(
                    "    const _: () = assert!(::core::mem::offset_of!({}, "
                    "{}) == {});",
                    decl.name, rust_field(field.name), field.abi_offset);
            }
            writer.blank();
        }
        for (const auto &decl : module.declarations) {
            if (decl.kind != declaration_kind::service) {
                continue;
            }
            const auto prefix{service_prefix(program, module_index, decl)};
            const auto service{service_id_of(program, module_index, decl)};
            writer.line("    #[repr(C)]");
            writer.line("    pub struct {}_vtable {{", prefix);
            writer.line("        pub struct_size: u32,");
            writer.line("        pub abi_major: u32,");
            writer.line("        pub abi_minor: u32,");
            writer.line("        pub abi_patch: u32,");
            for (const auto &method : decl.methods) {
                std::string parameters{"self_: *mut ::core::ffi::c_void"};
                for (const auto &parameter : method.parameters) {
                    parameters += ", ";
                    parameters += rust_field(parameter.name) + ": ";
                    if (parameter.out) {
                        parameters += "*mut " + rust_type_of(parameter.type);
                    } else if (parameter.type.kind ==
                                   resolved_type::category::named &&
                               decl_of(program, parameter.type.module_index,
                                       parameter.type.decl_index)
                                       .kind == declaration_kind::structure) {
                        parameters += "*const " + rust_type_of(parameter.type);
                    } else {
                        parameters += rust_type_of(parameter.type);
                    }
                }
                std::string return_type{"RefrainStatus"};
                if (method.can_fail) {
                    if (!method.result_type.is_void()) {
                        parameters += ", out_value: *mut " +
                                      rust_type_of(method.result_type);
                    }
                } else if (!method.result_type.is_void()) {
                    return_type = rust_type_of(method.result_type);
                } else {
                    return_type = "()";
                }
                writer.line(
                    "        pub {}: Option<unsafe extern \"C\" fn({}) -> "
                    "{}>,",
                    rust_method(method.name), parameters, return_type);
            }
            writer.line("    }}");
            writer.blank();
            writer.line("    pub const {}_SERVICE_ID: &str = \"{}\";",
                        emit::upper(prefix), emit::escape_text(service));
            writer.line("    pub const {}_ABI_MAJOR: u32 = {};",
                        emit::upper(prefix), decl.service_version_major);
            writer.line("    pub const {}_ABI_MINOR: u32 = {};",
                        emit::upper(prefix), decl.service_version_minor);
            writer.line("    pub const {}_ABI_PATCH: u32 = {};",
                        emit::upper(prefix), decl.service_version_patch);
            writer.blank();
        }
        writer.line("}} // pub mod {}", module_last_segment(module.name));
        writer.blank();
    }
    auto text{writer.take()};
    while (text.size() >= 2 && text.ends_with("\n\n")) {
        text.pop_back();
    }
    return emit::generated_file{path, std::move(text)};
}

} // namespace refrain::schema
