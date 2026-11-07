// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <schema/code_writer.hpp>
#include <schema/emit.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace refrain::schema {
namespace {

[[nodiscard]] auto decl_cpp_name(const model &program,
                                 const resolved_type &type) -> std::string {
    const auto &module{program.modules[type.module_index]};
    return "::" + emit::cpp_namespace(module.name) +
           "::" + module.declarations[type.decl_index].name;
}

[[nodiscard]] auto rsc_cpp_type(const model &program, const resolved_type &type)
    -> std::string {
    switch (type.kind) {
    case resolved_type::category::primitive:
        switch (type.primitive) {
        case primitive_kind::u8:
            return "std::uint8_t";
        case primitive_kind::u16:
            return "std::uint16_t";
        case primitive_kind::u32:
            return "std::uint32_t";
        case primitive_kind::u64:
            return "std::uint64_t";
        case primitive_kind::i8:
            return "std::int8_t";
        case primitive_kind::i16:
            return "std::int16_t";
        case primitive_kind::i32:
            return "std::int32_t";
        case primitive_kind::i64:
            return "std::int64_t";
        case primitive_kind::f32:
            return "float";
        case primitive_kind::f64:
            return "double";
        case primitive_kind::boolean:
            return "bool";
        case primitive_kind::string:
            return "std::string";
        case primitive_kind::bytes:
            return "std::vector<std::byte>";
        case primitive_kind::identifier:
            return "::refrain::data::resource_location";
        case primitive_kind::uuid:
            return "::refrain::net::uuid";
        case primitive_kind::text_component:
            return "::refrain::net::text_component";
        case primitive_kind::item_stack:
            return "::refrain::net::item_stack";
        case primitive_kind::lp_vec3:
            return "::refrain::net::lp_vec3";
        case primitive_kind::nbt:
            return "::refrain::net::nbt_blob";
        case primitive_kind::varint_id:
            return "std::uint32_t";
        case primitive_kind::message_signature_packed:
            return "::refrain::net::message_signature_packed";
        case primitive_kind::filter_mask:
            return "::refrain::net::filter_mask";
        case primitive_kind::void_type:
            return "void";
        }
        return "void";
    case resolved_type::category::named:
        return decl_cpp_name(program, type);
    case resolved_type::category::array:
    case resolved_type::category::repeated:
    case resolved_type::category::slice:
        // RSC data types own their storage; slices become vectors.
        return "std::vector<" + rsc_cpp_type(program, type.arguments.front()) +
               ">";
    case resolved_type::category::optional:
        return "std::optional<" +
               rsc_cpp_type(program, type.arguments.front()) + ">";
    case resolved_type::category::map:
        return "std::map<" + rsc_cpp_type(program, type.arguments[0]) + ", " +
               rsc_cpp_type(program, type.arguments[1]) + ">";
    case resolved_type::category::either:
        return "std::variant<" + rsc_cpp_type(program, type.arguments[0]) +
               ", " + rsc_cpp_type(program, type.arguments[1]) + ">";
    case resolved_type::category::holder:
        return "std::variant<std::uint32_t, " +
               rsc_cpp_type(program, type.arguments[0]) + ">";
    case resolved_type::category::handle:
        std::unreachable(); // sema rejects handles in RSC data schemas
    }
    std::unreachable();
}

[[nodiscard]] auto is_rsc_struct(const decl_model &decl) noexcept -> bool {
    return decl.kind == declaration_kind::structure ||
           decl.kind == declaration_kind::record ||
           decl.kind == declaration_kind::packet;
}

void collect_includes(const model &program, const resolved_type &type,
                      std::set<std::string> &includes) {
    if (type.kind == resolved_type::category::named) {
        const auto &decl{
            program.modules[type.module_index].declarations[type.decl_index]};
        if (!decl.source_path.empty()) {
            includes.insert(emit::rsc_cpp_include(decl.source_path));
        }
        return;
    }
    for (const auto &argument : type.arguments) {
        collect_includes(program, argument, includes);
    }
}

[[nodiscard]] auto file_declarations(const model &program,
                                     std::uint32_t module_index,
                                     std::string_view source_path)
    -> std::vector<const decl_model *> {
    std::vector<const decl_model *> declarations;
    for (const auto &decl : program.modules[module_index].declarations) {
        if (decl.source_path == source_path) {
            declarations.push_back(&decl);
        }
    }
    return declarations;
}

/// Order struct-like declarations so that same-file by-value dependencies are
/// emitted first (cross-file dependencies are handled with #include).
[[nodiscard]] auto
ordered_declarations(const model &program,
                     const std::vector<const decl_model *> &declarations,
                     std::uint32_t module_index, std::string_view source_path)
    -> std::vector<const decl_model *> {
    std::map<const decl_model *, std::vector<const decl_model *>> dependencies;
    // Container fields (`array<T>`, `optional<T>`, `slice<T>`,
    // `repeated<T>`) render as std::vector/std::optional/<T> and therefore need
    // T's definition first too; recursing only into by-value named fields
    // produced headers that did not compile when the element type was declared
    // later in the same file.
    const auto dependencies_of = [&](const resolved_type &type,
                                     std::vector<const decl_model *> &out) {
        const auto visit = [&](const auto &self,
                               const resolved_type &node) -> void {
            if (node.kind == resolved_type::category::named) {
                const auto &target{program.modules[node.module_index]
                                       .declarations[node.decl_index]};
                if (target.source_path == source_path &&
                    node.module_index == module_index &&
                    is_rsc_struct(target)) {
                    out.push_back(&target);
                }
                return;
            }
            for (const auto &argument : node.arguments) {
                self(self, argument);
            }
        };
        visit(visit, type);
    };
    for (const auto *decl : declarations) {
        if (!is_rsc_struct(*decl)) {
            continue;
        }
        for (const auto &field : decl->fields) {
            dependencies_of(field.type, dependencies[decl]);
        }
    }

    std::vector<const decl_model *> order;
    std::set<const decl_model *> visited;
    const auto visit = [&](const auto &self, const decl_model *decl) -> void {
        if (!visited.insert(decl).second) {
            return;
        }
        const auto found{dependencies.find(decl)};
        if (found != dependencies.end()) {
            for (const auto *dependency : found->second) {
                self(self, dependency);
            }
        }
        order.push_back(decl);
    };
    for (const auto *decl : declarations) {
        if (is_rsc_struct(*decl)) {
            visit(visit, decl);
        }
    }
    return order;
}

[[nodiscard]] auto emit_field(const model &program, const field_model &field)
    -> std::string {
    code_writer writer;
    writer.raw(emit::doc_block(field.documentation, "    /// "));
    writer.raw(emit::attribute_comment(field.attributes, "    "));
    const auto default_suffix{field.default_value.has_value()
                                  ? " // default: " +
                                        emit::escape_text(*field.default_value)
                                  : std::string{}};
    writer.line("    {} {};{}", rsc_cpp_type(program, field.type), field.name,
                default_suffix);
    return writer.take();
}

[[nodiscard]] auto emit_struct(const model &program, std::uint32_t module_index,
                               const decl_model &decl) -> std::string {
    code_writer writer;
    writer.raw(emit::doc_block(decl.documentation, "/// "));
    writer.raw(emit::attribute_comment(decl.attributes));
    writer.line("struct {} {{", decl.name);
    if (decl.kind == declaration_kind::packet && decl.packet_id.has_value()) {
        writer.line("    static constexpr std::uint16_t packet_id = 0x{:X};",
                    static_cast<std::uint64_t>(*decl.packet_id));
        writer.line(
            R"(    static constexpr std::string_view packet_name = "{}";)",
            emit::escape_text(decl.name));
        writer.blank();
    }
    if (decl.kind == declaration_kind::record) {
        writer.line(
            "    /// RWF record marker; field order is the storage ABI.");
    }
    for (const auto &field : decl.fields) {
        writer.raw(emit_field(program, field));
    }
    writer.line("}};");
    writer.blank();
    static_cast<void>(module_index);
    return writer.take();
}

[[nodiscard]] auto emit_enum(const decl_model &decl) -> std::string {
    code_writer writer;
    writer.raw(emit::doc_block(decl.documentation, "/// "));
    writer.raw(emit::attribute_comment(decl.attributes));
    writer.line("enum class {} : {} {{", decl.name,
                emit::cpp_underlying_name(decl));
    for (const auto &entry : decl.entries) {
        writer.raw(emit::doc_block(entry.documentation, "    /// "));
        writer.line("    {} = {},", entry.name, entry.value.value_or(0));
    }
    writer.line("}};");
    writer.blank();
    return writer.take();
}

[[nodiscard]] auto declaration_name(const model &program,
                                    const resolved_type &type) -> std::string {
    return program.modules[type.module_index]
        .declarations[type.decl_index]
        .name;
}

[[nodiscard]] auto declaration_namespace(const model &program,
                                         const resolved_type &type)
    -> std::string {
    return emit::cpp_namespace(program.modules[type.module_index].name);
}

[[nodiscard]] auto declaration_is_enum(const model &program,
                                       const resolved_type &type) -> bool {
    const auto kind{
        program.modules[type.module_index].declarations[type.decl_index].kind};
    return kind == declaration_kind::enumeration ||
           kind == declaration_kind::flags;
}

[[nodiscard]] auto is_struct_like(const decl_model &decl) noexcept -> bool {
    return decl.kind == declaration_kind::structure ||
           decl.kind == declaration_kind::record ||
           decl.kind == declaration_kind::packet;
}

void emit_encode_integer(code_writer &writer, primitive_kind kind,
                         integer_wire wire, const std::string &expression) {
    const auto unsigned_expression{"static_cast<std::uint32_t>(" + expression +
                                   ")"};
    const auto signed_expression{"static_cast<std::int64_t>(" + expression +
                                 ")"};
    switch (wire) {
    case integer_wire::varint:
        writer.line("out.write_varint({});", unsigned_expression);
        return;
    case integer_wire::varlong:
        writer.line("out.write_varlong(static_cast<std::uint64_t>({}));",
                    expression);
        return;
    case integer_wire::fixed:
        switch (kind) {
        case primitive_kind::u8:
        case primitive_kind::i8:
            writer.line("out.write_u8(static_cast<std::uint8_t>({}));",
                        expression);
            return;
        case primitive_kind::u16:
        case primitive_kind::i16:
            writer.line("out.write_u16(static_cast<std::uint16_t>({}));",
                        expression);
            return;
        case primitive_kind::u32:
        case primitive_kind::i32:
            writer.line("out.write_i32(static_cast<std::int32_t>({}));",
                        expression);
            return;
        case primitive_kind::u64:
        case primitive_kind::i64:
            writer.line("out.write_i64(static_cast<std::int64_t>({}));",
                        expression);
            return;
        default:
            std::unreachable();
        }
    case integer_wire::natural:
        break;
    }
    switch (kind) {
    case primitive_kind::u8:
        writer.line("out.write_u8({});", expression);
        return;
    case primitive_kind::u16:
        writer.line("out.write_u16({});", expression);
        return;
    case primitive_kind::u32:
    case primitive_kind::u64:
        writer.line("out.write_varint({});", unsigned_expression);
        return;
    case primitive_kind::i8:
    case primitive_kind::i16:
    case primitive_kind::i32:
    case primitive_kind::i64:
        writer.line("out.write_i64({});", signed_expression);
        return;
    default:
        std::unreachable();
    }
}

[[nodiscard]] auto integer_read_call(primitive_kind kind, integer_wire wire)
    -> std::string {
    switch (wire) {
    case integer_wire::varint:
        return "in.read_varint()";
    case integer_wire::varlong:
        return "in.read_varlong()";
    case integer_wire::fixed:
        switch (kind) {
        case primitive_kind::u8:
        case primitive_kind::i8:
            return "in.read_u8()";
        case primitive_kind::u16:
        case primitive_kind::i16:
            return "in.read_u16()";
        case primitive_kind::u32:
        case primitive_kind::i32:
            return "in.read_i32()";
        case primitive_kind::u64:
        case primitive_kind::i64:
            return "in.read_i64()";
        default:
            std::unreachable();
        }
    case integer_wire::natural:
        break;
    }
    switch (kind) {
    case primitive_kind::u8:
        return "in.read_u8()";
    case primitive_kind::u16:
        return "in.read_u16()";
    case primitive_kind::u32:
    case primitive_kind::u64:
        return "in.read_varint()";
    case primitive_kind::i8:
    case primitive_kind::i16:
    case primitive_kind::i32:
    case primitive_kind::i64:
        return "in.read_i64()";
    default:
        std::unreachable();
    }
}

void emit_decode_integer(code_writer &writer, primitive_kind kind,
                         integer_wire wire, const std::string &target,
                         std::size_t depth) {
    writer.line("auto value_{} = {};", depth, integer_read_call(kind, wire));
    writer.line(
        "if (!value_{}) return std::unexpected{{std::move(value_{}.error())}};",
        depth, depth);
    writer.line("{} = static_cast<decltype({})>(*value_{});", target, target,
                depth);
}

struct wire_hints {
    integer_wire integer{integer_wire::natural};
    std::optional<std::uint64_t> maximum;
    std::optional<std::uint64_t> fixed_bytes;
    std::optional<std::uint64_t> fixed_count;
    bool optional_varint{false};
    bool rest{false};
};

[[nodiscard]] auto field_hints(const field_model &field) -> wire_hints {
    wire_hints hints;
    hints.integer = field.wire;
    hints.maximum = field.wire_max;
    hints.fixed_bytes = field.wire_fixed_bytes;
    hints.optional_varint = field.wire_optional_varint;
    hints.rest = field.wire_rest;
    hints.fixed_count = field.wire_fixed_count;
    return hints;
}

/// Hints for array elements: the integer encoding applies, while bounds and
/// fixed lengths belong to the container.
[[nodiscard]] auto element_hints(const wire_hints &hints) -> wire_hints {
    wire_hints result;
    result.integer = hints.integer;
    return result;
}

/// Hints for an optional payload: fixed-length payloads keep their length.
[[nodiscard]] auto payload_hints(const wire_hints &hints) -> wire_hints {
    wire_hints result;
    result.integer = hints.integer;
    result.fixed_bytes = hints.fixed_bytes;
    return result;
}

void emit_encode_value(code_writer &writer, const model &program,
                       const resolved_type &type, std::string expression,
                       std::size_t depth, const wire_hints &hints,
                       const std::string &label) {
    switch (type.kind) {
    case resolved_type::category::primitive:
        switch (type.primitive) {
        case primitive_kind::u8:
        case primitive_kind::u16:
        case primitive_kind::u32:
        case primitive_kind::u64:
        case primitive_kind::i8:
        case primitive_kind::i16:
        case primitive_kind::i32:
        case primitive_kind::i64:
            emit_encode_integer(writer, type.primitive, hints.integer,
                                expression);
            return;
        case primitive_kind::f32:
            writer.line("out.write_f32({});", expression);
            return;
        case primitive_kind::f64:
            writer.line("out.write_f64({});", expression);
            return;
        case primitive_kind::boolean:
            writer.line("out.write_bool({});", expression);
            return;
        case primitive_kind::string:
            writer.line(
                "if (auto written = out.write_string({}, {}); !written) {{",
                expression, hints.maximum.value_or(32767));
            writer.indent();
            writer.line("return written;");
            writer.dedent();
            writer.line("}}");
            return;
        case primitive_kind::bytes:
            if (hints.rest) {
                writer.line("out.write_bytes({});", expression);
                return;
            }
            if (hints.fixed_bytes.has_value()) {
                writer.line("if ({}.size() != {}) {{", expression,
                            *hints.fixed_bytes);
                writer.indent();
                writer.line("return "
                            "std::unexpected{{::refrain::make_error(::refrain::"
                            "error_code::"
                            "invalid_argument, \"field '{}' must be exactly {} "
                            "byte(s)\")}};",
                            label, *hints.fixed_bytes);
                writer.dedent();
                writer.line("}}");
                writer.line(
                    "out.write_bytes(std::span<const std::byte>{{{} }});",
                    expression);
                return;
            }
            if (hints.maximum.has_value()) {
                writer.line("if ({}.size() > {}) {{", expression,
                            *hints.maximum);
                writer.indent();
                writer.line(
                    "return "
                    "std::unexpected{{::refrain::make_error(::refrain::error_"
                    "code::"
                    "invalid_argument, \"field '{}' exceeds its declared "
                    "maximum\")}};",
                    label);
                writer.dedent();
                writer.line("}}");
            }
            writer.line(
                "out.write_varint(static_cast<std::uint32_t>({}.size()));",
                expression);
            writer.line("out.write_bytes(std::span<const std::byte>{{{} }});",
                        expression);
            return;
        case primitive_kind::identifier:
            writer.line(
                "if (auto written = out.write_identifier({}); !written) {{",
                expression);
            writer.indent();
            writer.line("return written;");
            writer.dedent();
            writer.line("}}");
            return;
        case primitive_kind::uuid:
            writer.line("if (auto written = out.write_uuid({}); !written) {{",
                        expression);
            writer.indent();
            writer.line("return written;");
            writer.dedent();
            writer.line("}}");
            return;
        case primitive_kind::text_component:
            writer.line(
                "if (auto written = ::refrain::net::encode_text_component("
                "out, {}); !written) {{",
                expression);
            writer.indent();
            writer.line("return written;");
            writer.dedent();
            writer.line("}}");
            return;
        case primitive_kind::item_stack:
            writer.line("if (auto written = ::refrain::net::encode_item_stack("
                        "out, {}); !written) {{",
                        expression);
            writer.indent();
            writer.line("return written;");
            writer.dedent();
            writer.line("}}");
            return;
        case primitive_kind::lp_vec3:
            writer.line("if (auto written = ::refrain::net::encode_lp_vec3("
                        "out, {}); !written) {{",
                        expression);
            writer.indent();
            writer.line("return written;");
            writer.dedent();
            writer.line("}}");
            return;
        case primitive_kind::nbt:
            writer.line("if (auto written = ::refrain::net::encode_nbt("
                        "out, {}); !written) {{",
                        expression);
            writer.indent();
            writer.line("return written;");
            writer.dedent();
            writer.line("}}");
            return;
        case primitive_kind::varint_id:
            writer.line("out.write_varint({});", expression);
            return;
        case primitive_kind::message_signature_packed:
            writer.line("if (auto written = "
                        "::refrain::net::encode_message_signature_packed("
                        "out, {}); !written) {{",
                        expression);
            writer.indent();
            writer.line("return written;");
            writer.dedent();
            writer.line("}}");
            return;
        case primitive_kind::filter_mask:
            writer.line("if (auto written = ::refrain::net::encode_filter_mask("
                        "out, {}); !written) {{",
                        expression);
            writer.indent();
            writer.line("return written;");
            writer.dedent();
            writer.line("}}");
            return;
        case primitive_kind::void_type:
            return;
        }
        return;
    case resolved_type::category::named:
        if (declaration_is_enum(program, type)) {
            writer.line("out.write_varint(static_cast<std::uint32_t>({}));",
                        expression);
        } else {
            writer.line(
                "if (auto written = ::{}::encode_{}(out, {}); !written) {{",
                declaration_namespace(program, type),
                declaration_name(program, type), expression);
            writer.indent();
            writer.line("return written;");
            writer.dedent();
            writer.line("}}");
        }
        return;
    case resolved_type::category::array:
    case resolved_type::category::slice:
    case resolved_type::category::repeated: {
        if (hints.fixed_count.has_value()) {
            writer.line("if ({}.size() != {}) {{", expression,
                        *hints.fixed_count);
            writer.indent();
            writer.line(
                "return "
                "std::unexpected{{::refrain::make_error(::refrain::error_code::"
                "invalid_argument, \"field '{}' must have exactly {} "
                "element(s)\")}};",
                label, *hints.fixed_count);
            writer.dedent();
            writer.line("}}");
        } else {
            if (hints.maximum.has_value()) {
                writer.line("if ({}.size() > {}) {{", expression,
                            *hints.maximum);
                writer.indent();
                writer.line(
                    "return "
                    "std::unexpected{{::refrain::make_error(::refrain::error_"
                    "code::invalid_argument, \"field '{}' exceeds its "
                    "declared maximum\")}};",
                    label);
                writer.dedent();
                writer.line("}}");
            }
            writer.line(
                "out.write_varint(static_cast<std::uint32_t>({}.size()));",
                expression);
        }
        const auto item{"item_" + std::to_string(depth)};
        writer.line("for (const auto &{} : {}) {{", item, expression);
        writer.indent();
        emit_encode_value(writer, program, type.arguments.front(), item,
                          depth + 1, element_hints(hints), label);
        writer.dedent();
        writer.line("}}");
        return;
    }
    case resolved_type::category::map: {
        if (hints.maximum.has_value()) {
            writer.line("if ({}.size() > {}) {{", expression, *hints.maximum);
            writer.indent();
            writer.line(
                "return "
                "std::unexpected{{::refrain::make_error(::refrain::error_code::"
                "invalid_argument, \"field '{}' exceeds its declared "
                "maximum\")}};",
                label);
            writer.dedent();
            writer.line("}}");
        }
        writer.line("out.write_varint(static_cast<std::uint32_t>({}.size()));",
                    expression);
        const auto entry{"entry_" + std::to_string(depth)};
        writer.line("for (const auto &{} : {}) {{", entry, expression);
        writer.indent();
        emit_encode_value(writer, program, type.arguments[0], entry + ".first",
                          depth + 1, element_hints(hints), label);
        emit_encode_value(writer, program, type.arguments[1], entry + ".second",
                          depth + 1, element_hints(hints), label);
        writer.dedent();
        writer.line("}}");
        return;
    }
    case resolved_type::category::holder: {
        writer.line("if (std::holds_alternative<std::uint32_t>({})) {{",
                    expression);
        writer.indent();
        writer.line("out.write_varint(std::get<0>({}));", expression);
        writer.dedent();
        writer.line("}} else {{");
        writer.indent();
        writer.line("out.write_varint(0);");
        emit_encode_value(writer, program, type.arguments[0],
                          "std::get<1>(" + expression + ")", depth + 1,
                          element_hints(hints), label);
        writer.dedent();
        writer.line("}}");
        return;
    }
    case resolved_type::category::either: {
        writer.line("out.write_bool(std::holds_alternative<{}>({}));",
                    rsc_cpp_type(program, type.arguments[0]), expression);
        writer.line("if (std::holds_alternative<{}>({})) {{",
                    rsc_cpp_type(program, type.arguments[0]), expression);
        writer.indent();
        emit_encode_value(writer, program, type.arguments[0],
                          "std::get<0>(" + expression + ")", depth + 1,
                          element_hints(hints), label);
        writer.dedent();
        writer.line("}} else {{");
        writer.indent();
        emit_encode_value(writer, program, type.arguments[1],
                          "std::get<1>(" + expression + ")", depth + 1,
                          element_hints(hints), label);
        writer.dedent();
        writer.line("}}");
        return;
    }
    case resolved_type::category::optional:
        if (hints.optional_varint) {
            writer.line("out.write_varint({}.has_value() ? "
                        "static_cast<std::uint32_t>({}.value()) + 1U : 0U);",
                        expression, expression);
            return;
        }
        writer.line("out.write_bool({}.has_value());", expression);
        writer.line("if ({}.has_value()) {{", expression);
        writer.indent();
        emit_encode_value(writer, program, type.arguments.front(),
                          expression + ".value()", depth + 1,
                          payload_hints(hints), label);
        writer.dedent();
        writer.line("}}");
        return;
    case resolved_type::category::handle:
        return;
    }
}

void emit_decode_value(code_writer &writer, const model &program,
                       const resolved_type &type, std::string target,
                       std::size_t depth, const wire_hints &hints,
                       const std::string &label) {
    switch (type.kind) {
    case resolved_type::category::primitive:
        switch (type.primitive) {
        case primitive_kind::u8:
        case primitive_kind::u16:
        case primitive_kind::u32:
        case primitive_kind::u64:
        case primitive_kind::i8:
        case primitive_kind::i16:
        case primitive_kind::i32:
        case primitive_kind::i64:
            emit_decode_integer(writer, type.primitive, hints.integer, target,
                                depth);
            return;
        case primitive_kind::f32:
            writer.line("auto value_{} = in.read_f32();", depth);
            writer.line("if (!value_{}) return "
                        "std::unexpected{{std::move(value_{}.error())}};",
                        depth, depth);
            writer.line("{} = *value_{};", target, depth);
            return;
        case primitive_kind::f64:
            writer.line("auto value_{} = in.read_f64();", depth);
            writer.line("if (!value_{}) return "
                        "std::unexpected{{std::move(value_{}.error())}};",
                        depth, depth);
            writer.line("{} = *value_{};", target, depth);
            return;
        case primitive_kind::boolean:
            writer.line("auto value_{} = in.read_bool();", depth);
            writer.line("if (!value_{}) return "
                        "std::unexpected{{std::move(value_{}.error())}};",
                        depth, depth);
            writer.line("{} = *value_{};", target, depth);
            return;
        case primitive_kind::string:
            writer.line("auto value_{} = in.read_string({});", depth,
                        hints.maximum.value_or(32767));
            writer.line("if (!value_{}) return "
                        "std::unexpected{{std::move(value_{}.error())}};",
                        depth, depth);
            writer.line("{} = std::move(*value_{});", target, depth);
            return;
        case primitive_kind::bytes:
            if (hints.rest) {
                writer.line("auto value_{} = in.read_bytes(in.remaining());",
                            depth);
                writer.line("if (!value_{}) return "
                            "std::unexpected{{std::move(value_{}.error())}};",
                            depth, depth);
                writer.line("{} = std::vector<std::byte>{{value_{}->begin(), "
                            "value_{}->end()}};",
                            target, depth, depth);
                return;
            }
            if (hints.fixed_bytes.has_value()) {
                writer.line("auto value_{} = in.read_bytes({});", depth,
                            *hints.fixed_bytes);
                writer.line("if (!value_{}) return "
                            "std::unexpected{{std::move(value_{}.error())}};",
                            depth, depth);
                writer.line("{} = std::vector<std::byte>{{value_{}->begin(), "
                            "value_{}->end()}};",
                            target, depth, depth);
                return;
            }
            writer.line("auto length_{} = in.read_varint();", depth);
            writer.line("if (!length_{}) return "
                        "std::unexpected{{std::move(length_{}.error())}};",
                        depth, depth);
            if (hints.maximum.has_value()) {
                writer.line("if (*length_{} > {}) {{", depth, *hints.maximum);
                writer.indent();
                writer.line("return "
                            "std::unexpected{{::refrain::make_error(::refrain::"
                            "error_code::"
                            "parse_error, \"field '{}' exceeds its declared "
                            "maximum\")}};",
                            label);
                writer.dedent();
                writer.line("}}");
            }
            writer.line("auto value_{} = in.read_bytes(*length_{});", depth,
                        depth);
            writer.line("if (!value_{}) return "
                        "std::unexpected{{std::move(value_{}.error())}};",
                        depth, depth);
            writer.line("{} = std::vector<std::byte>{{value_{}->begin(), "
                        "value_{}->end()}};",
                        target, depth, depth);
            return;
        case primitive_kind::identifier:
            writer.line("auto value_{} = in.read_identifier();", depth);
            writer.line("if (!value_{}) return "
                        "std::unexpected{{std::move(value_{}.error())}};",
                        depth, depth);
            writer.line("{} = std::move(*value_{});", target, depth);
            return;
        case primitive_kind::uuid:
            writer.line("auto value_{} = in.read_uuid();", depth);
            writer.line("if (!value_{}) return "
                        "std::unexpected{{std::move(value_{}.error())}};",
                        depth, depth);
            writer.line("{} = *value_{};", target, depth);
            return;
        case primitive_kind::text_component:
            writer.line(
                "if (auto decoded = ::refrain::net::decode_text_component("
                "in, {}); !decoded) {{",
                target);
            writer.indent();
            writer.line(
                "return std::unexpected{{std::move(decoded.error())}};");
            writer.dedent();
            writer.line("}}");
            return;
        case primitive_kind::item_stack:
            writer.line("if (auto decoded = ::refrain::net::decode_item_stack("
                        "in, {}); !decoded) {{",
                        target);
            writer.indent();
            writer.line(
                "return std::unexpected{{std::move(decoded.error())}};");
            writer.dedent();
            writer.line("}}");
            return;
        case primitive_kind::lp_vec3:
            writer.line("if (auto decoded = ::refrain::net::decode_lp_vec3("
                        "in, {}); !decoded) {{",
                        target);
            writer.indent();
            writer.line(
                "return std::unexpected{{std::move(decoded.error())}};");
            writer.dedent();
            writer.line("}}");
            return;
        case primitive_kind::nbt:
            writer.line("if (auto decoded = ::refrain::net::decode_nbt("
                        "in, {}); !decoded) {{",
                        target);
            writer.indent();
            writer.line(
                "return std::unexpected{{std::move(decoded.error())}};");
            writer.dedent();
            writer.line("}}");
            return;
        case primitive_kind::varint_id:
            writer.line("auto value_{} = in.read_varint();", depth);
            writer.line("if (!value_{}) return "
                        "std::unexpected{{std::move(value_{}.error())}};",
                        depth, depth);
            writer.line("{} = *value_{};", target, depth);
            return;
        case primitive_kind::message_signature_packed:
            writer.line("if (auto decoded = "
                        "::refrain::net::decode_message_signature_packed("
                        "in, {}); !decoded) {{",
                        target);
            writer.indent();
            writer.line(
                "return std::unexpected{{std::move(decoded.error())}};");
            writer.dedent();
            writer.line("}}");
            return;
        case primitive_kind::filter_mask:
            writer.line("if (auto decoded = ::refrain::net::decode_filter_mask("
                        "in, {}); !decoded) {{",
                        target);
            writer.indent();
            writer.line(
                "return std::unexpected{{std::move(decoded.error())}};");
            writer.dedent();
            writer.line("}}");
            return;
        case primitive_kind::void_type:
            return;
        }
        return;
    case resolved_type::category::named:
        if (declaration_is_enum(program, type)) {
            writer.line("auto value_{} = in.read_varint();", depth);
            writer.line("if (!value_{}) return "
                        "std::unexpected{{std::move(value_{}.error())}};",
                        depth, depth);
            writer.line("{} = static_cast<decltype({})>(*value_{});", target,
                        target, depth);
        } else {
            writer.line(
                "if (auto decoded = ::{}::decode_{}(in, {}); !decoded) {{",
                declaration_namespace(program, type),
                declaration_name(program, type), target);
            writer.indent();
            writer.line(
                "return std::unexpected{{std::move(decoded.error())}};");
            writer.dedent();
            writer.line("}}");
        }
        return;
    case resolved_type::category::array:
    case resolved_type::category::slice:
    case resolved_type::category::repeated: {
        writer.line("{}.clear();", target);
        const auto index{"index_" + std::to_string(depth)};
        const auto item{"item_" + std::to_string(depth)};
        if (hints.fixed_count.has_value()) {
            writer.line("{}.reserve({});", target, *hints.fixed_count);
            writer.line("for (std::uint32_t {} = 0; {} < {}; ++{}) {{", index,
                        index, *hints.fixed_count, index);
        } else {
            writer.line("auto count_{} = in.read_varint();", depth);
            writer.line("if (!count_{}) return "
                        "std::unexpected{{std::move(count_{}.error())}};",
                        depth, depth);
            if (hints.maximum.has_value()) {
                writer.line("if (*count_{} > {}) {{", depth, *hints.maximum);
                writer.indent();
                writer.line(
                    "return "
                    "std::unexpected{{::refrain::make_error(::refrain::error_"
                    "code::parse_error, \"field '{}' exceeds its declared "
                    "maximum\")}};",
                    label);
                writer.dedent();
                writer.line("}}");
            }
            writer.line("for (std::uint32_t {} = 0; {} < *count_{}; ++{}) {{",
                        index, index, depth, index);
        }
        writer.indent();
        writer.line("{} value_{}{{}};",
                    rsc_cpp_type(program, type.arguments.front()), depth);
        emit_decode_value(writer, program, type.arguments.front(),
                          "value_" + std::to_string(depth), depth + 1,
                          element_hints(hints), label);
        writer.line("{}.push_back(std::move(value_{}));", target, depth);
        writer.dedent();
        writer.line("}}");
        return;
    }
    case resolved_type::category::map: {
        writer.line("auto count_{} = in.read_varint();", depth);
        writer.line("if (!count_{}) return "
                    "std::unexpected{{std::move(count_{}.error())}};",
                    depth, depth);
        if (hints.maximum.has_value()) {
            writer.line("if (*count_{} > {}) {{", depth, *hints.maximum);
            writer.indent();
            writer.line(
                "return "
                "std::unexpected{{::refrain::make_error(::refrain::error_code::"
                "parse_error, \"field '{}' exceeds its declared maximum\")}};",
                label);
            writer.dedent();
            writer.line("}}");
        }
        writer.line("{}.clear();", target);
        const auto index{"index_" + std::to_string(depth)};
        const auto key{"map_key_" + std::to_string(depth)};
        const auto value{"map_value_" + std::to_string(depth)};
        writer.line("for (std::uint32_t {} = 0; {} < *count_{}; ++{}) {{",
                    index, index, depth, index);
        writer.indent();
        writer.line("{} {}{{}};", rsc_cpp_type(program, type.arguments[0]),
                    key);
        emit_decode_value(writer, program, type.arguments[0], key, depth + 1,
                          element_hints(hints), label);
        writer.line("{} {}{{}};", rsc_cpp_type(program, type.arguments[1]),
                    value);
        emit_decode_value(writer, program, type.arguments[1], value, depth + 2,
                          element_hints(hints), label);
        writer.line("{}.emplace(std::move({}), std::move({}));", target, key,
                    value);
        writer.dedent();
        writer.line("}}");
        return;
    }
    case resolved_type::category::holder: {
        writer.line("auto holder_{} = in.read_varint();", depth);
        writer.line("if (!holder_{}) return "
                    "std::unexpected{{std::move(holder_{}.error())}};",
                    depth, depth);
        writer.line("if (*holder_{} != 0) {{", depth);
        writer.indent();
        writer.line("{}.template emplace<0>(*holder_{});", target, depth);
        writer.dedent();
        writer.line("}} else {{");
        writer.indent();
        writer.line("{} value_{}{{}};",
                    rsc_cpp_type(program, type.arguments[0]), depth);
        emit_decode_value(writer, program, type.arguments[0],
                          "value_" + std::to_string(depth), depth + 1,
                          element_hints(hints), label);
        writer.line("{}.template emplace<1>(std::move(value_{}));", target,
                    depth);
        writer.dedent();
        writer.line("}}");
        return;
    }
    case resolved_type::category::either: {
        writer.line("auto left_{} = in.read_bool();", depth);
        writer.line("if (!left_{}) return "
                    "std::unexpected{{std::move(left_{}.error())}};",
                    depth, depth);
        writer.line("if (*left_{}) {{", depth);
        writer.indent();
        writer.line("{} value_{}{{}};",
                    rsc_cpp_type(program, type.arguments[0]), depth);
        emit_decode_value(writer, program, type.arguments[0],
                          "value_" + std::to_string(depth), depth + 1,
                          element_hints(hints), label);
        writer.line("{}.template emplace<0>(std::move(value_{}));", target,
                    depth);
        writer.dedent();
        writer.line("}} else {{");
        writer.indent();
        writer.line("{} value_{}{{}};",
                    rsc_cpp_type(program, type.arguments[1]), depth);
        emit_decode_value(writer, program, type.arguments[1],
                          "value_" + std::to_string(depth), depth + 2,
                          element_hints(hints), label);
        writer.line("{}.template emplace<1>(std::move(value_{}));", target,
                    depth);
        writer.dedent();
        writer.line("}}");
        return;
    }
    case resolved_type::category::optional:
        if (hints.optional_varint) {
            writer.line("auto present_{} = in.read_varint();", depth);
            writer.line("if (!present_{}) return "
                        "std::unexpected{{std::move(present_{}.error())}};",
                        depth, depth);
            writer.line("if (*present_{} != 0) {{", depth);
            writer.indent();
            writer.line("{} = static_cast<{}>(*present_{} - 1U);", target,
                        rsc_cpp_type(program, type.arguments.front()), depth);
            writer.dedent();
            writer.line("}} else {{");
            writer.indent();
            writer.line("{}.reset();", target);
            writer.dedent();
            writer.line("}}");
            return;
        }
        writer.line("auto present_{} = in.read_bool();", depth);
        writer.line("if (!present_{}) return "
                    "std::unexpected{{std::move(present_{}.error())}};",
                    depth, depth);
        writer.line("if (*present_{}) {{", depth);
        writer.indent();
        writer.line("{} value_{}{{}};",
                    rsc_cpp_type(program, type.arguments.front()), depth);
        emit_decode_value(writer, program, type.arguments.front(),
                          "value_" + std::to_string(depth), depth + 1,
                          payload_hints(hints), label);
        writer.line("{} = std::move(value_{});", target, depth);
        writer.dedent();
        writer.line("}} else {{");
        writer.indent();
        writer.line("{}.reset();", target);
        writer.dedent();
        writer.line("}}");
        return;
    case resolved_type::category::handle:
        return;
    }
}

[[nodiscard]] auto emit_codecs(const model &program,
                               const std::vector<const decl_model *> &ordered)
    -> std::string {
    code_writer writer;
    for (const auto *decl : ordered) {
        if (!is_struct_like(*decl)) {
            continue;
        }
        writer.line(
            "[[nodiscard]] inline auto encode_{}(::refrain::net::writer &out, "
            "const {} &value) -> ::refrain::status {{",
            decl->name, decl->name);
        writer.indent();
        writer.line("static_cast<void>(out);");
        writer.line("static_cast<void>(value);");
        for (const auto &field : decl->fields) {
            writer.line("{{");
            writer.indent();
            emit_encode_value(writer, program, field.type,
                              "value." + field.name, 0, field_hints(field),
                              field.name);
            writer.dedent();
            writer.line("}}");
        }
        writer.line("return ::refrain::status_ok();");
        writer.dedent();
        writer.line("}}");
        writer.blank();
        writer.line(
            "[[nodiscard]] inline auto decode_{}(::refrain::net::reader &in, "
            "{} &value) -> ::refrain::status {{",
            decl->name, decl->name);
        writer.indent();
        writer.line("static_cast<void>(in);");
        writer.line("static_cast<void>(value);");
        for (const auto &field : decl->fields) {
            writer.line("{{");
            writer.indent();
            emit_decode_value(writer, program, field.type,
                              "value." + field.name, 0, field_hints(field),
                              field.name);
            writer.dedent();
            writer.line("}}");
        }
        writer.line("return ::refrain::status_ok();");
        writer.dedent();
        writer.line("}}");
        writer.blank();
    }
    return writer.take();
}

[[nodiscard]] auto emit_header(const model &program, std::uint32_t module_index,
                               std::string_view source_path,
                               std::string_view schema_hash) -> std::string {
    const auto &module{program.modules[module_index]};
    const auto declarations{
        file_declarations(program, module_index, source_path)};
    const auto path{emit::rsc_cpp_header_path(source_path)};
    const auto guard{emit::guard_for(path)};

    std::set<std::string> includes;
    bool needs_identifier{false};
    const auto uses_identifier = [&](const auto &self,
                                     const resolved_type &type) -> bool {
        if (type.kind == resolved_type::category::primitive) {
            return type.primitive == primitive_kind::identifier;
        }
        return std::ranges::any_of(type.arguments, [&](const auto &argument) {
            return self(self, argument);
        });
    };
    bool needs_wire{false};
    bool needs_map{false};
    bool needs_variant{false};
    const auto uses_variant = [&](const auto &self,
                                  const resolved_type &type) -> bool {
        if (type.kind == resolved_type::category::either ||
            type.kind == resolved_type::category::holder) {
            return true;
        }
        return std::ranges::any_of(type.arguments, [&](const auto &argument) {
            return self(self, argument);
        });
    };
    const auto uses_map = [&](const auto &self,
                              const resolved_type &type) -> bool {
        if (type.kind == resolved_type::category::map) {
            return true;
        }
        return std::ranges::any_of(type.arguments, [&](const auto &argument) {
            return self(self, argument);
        });
    };
    const auto uses_wire = [&](const auto &self,
                               const resolved_type &type) -> bool {
        if (type.kind == resolved_type::category::primitive) {
            return type.primitive == primitive_kind::text_component ||
                   type.primitive == primitive_kind::item_stack ||
                   type.primitive == primitive_kind::lp_vec3 ||
                   type.primitive == primitive_kind::nbt ||
                   type.primitive == primitive_kind::message_signature_packed;
        }
        return std::ranges::any_of(type.arguments, [&](const auto &argument) {
            return self(self, argument);
        });
    };
    for (const auto *decl : declarations) {
        for (const auto &field : decl->fields) {
            collect_includes(program, field.type, includes);
            needs_identifier = needs_identifier ||
                               uses_identifier(uses_identifier, field.type);
            needs_wire = needs_wire || uses_wire(uses_wire, field.type);
            needs_map = needs_map || uses_map(uses_map, field.type);
            needs_variant =
                needs_variant || uses_variant(uses_variant, field.type);
        }
    }
    includes.erase(emit::rsc_cpp_include(source_path));

    code_writer writer;
    writer.raw(emit::banner(schema_hash));
    writer.line("#ifndef {}", guard);
    writer.line("#define {}", guard);
    writer.blank();
    writer.line("#include <cstddef>");
    writer.line("#include <cstdint>");
    writer.line("#include <optional>");
    writer.line("#include <string>");
    writer.line("#include <vector>");
    writer.line("#include <refrain/core/error.hpp>");
    writer.line("#include <refrain/net/base.hpp>");
    if (needs_identifier) {
        writer.line("#include <refrain/data/resource_location.hpp>");
    }
    if (needs_wire) {
        writer.line("#include <refrain/net/wire.hpp>");
    }
    if (needs_map) {
        writer.line("#include <map>");
        writer.line("#include <utility>");
    }
    if (needs_variant) {
        writer.line("#include <variant>");
    }
    for (const auto &include : includes) {
        writer.line("#include {}", include);
    }
    writer.blank();
    writer.line("namespace {} {{", emit::cpp_namespace(module.name));
    writer.blank();

    for (const auto *decl : declarations) {
        if (decl->kind == declaration_kind::enumeration) {
            writer.raw(emit_enum(*decl));
        }
    }
    const auto ordered{
        ordered_declarations(program, declarations, module_index, source_path)};
    for (std::size_t index = 0; index < ordered.size(); ++index) {
        auto struct_text{emit_struct(program, module_index, *ordered[index])};
        if (index + 1 == ordered.size()) {
            struct_text.pop_back(); // trailing blank line
        }
        writer.raw(struct_text);
    }
    writer.raw(emit_codecs(program, ordered));

    writer.line("}} // namespace {}", emit::cpp_namespace(module.name));
    writer.blank();
    writer.line("#endif // {}", guard);
    return writer.take();
}

struct protocol_layout {
    std::string version;
    std::string name_space; // refrain::protocol::v777
    std::string stage;
    std::string flow;
};

[[nodiscard]] auto protocol_layout_of(std::string_view module)
    -> std::optional<protocol_layout> {
    static constexpr std::array stages{"handshake", "status", "login",
                                       "configuration", "play"};
    static constexpr std::array flows{"serverbound", "clientbound"};
    const auto parts{emit::split_module(module)};
    if (parts.size() != 5 || parts[0] != "refrain" || parts[1] != "protocol" ||
        !parts[2].starts_with('v')) {
        return std::nullopt;
    }
    const auto version{parts[2].substr(1)};
    const auto digits = [](std::string_view text) {
        return !text.empty() && std::ranges::all_of(text, [](char character) {
            return character >= '0' && character <= '9';
        });
    };
    if (!digits(version) ||
        std::ranges::find(stages, parts[3]) == stages.end() ||
        std::ranges::find(flows, parts[4]) == flows.end()) {
        return std::nullopt;
    }
    return protocol_layout{.version = std::string{version},
                           .name_space = "refrain::protocol::" + parts[2],
                           .stage = parts[3],
                           .flow = parts[4]};
}

[[nodiscard]] auto generated_include(std::string_view path) -> std::string {
    constexpr std::string_view prefix{"cpp/"};
    if (path.starts_with(prefix)) {
        return "<" + std::string{path.substr(prefix.size())} + ">";
    }
    return "<" + std::string{path} + ">";
}

struct catalog_packet {
    protocol_layout layout;
    std::string name;
    std::uint16_t id{0};
};

struct stage_catalog {
    protocol_layout layout;
    std::set<std::string> flow_headers;
    std::vector<catalog_packet> packets;
};

void text_line(code_writer &writer, std::string_view text) {
    writer.line("{}", text);
}

[[nodiscard]] auto emit_packet_header(const std::string &version,
                                      const std::string &name_space,
                                      std::string_view schema_hash)
    -> std::string {
    const auto dir{"cpp/refrain/schema/protocol/" + version};
    const auto guard{emit::guard_for(dir + "/packet.hpp")};
    code_writer writer;
    writer.raw(emit::banner(schema_hash));
    writer.line("#ifndef {}", guard);
    writer.line("#define {}", guard);
    writer.blank();
    text_line(writer, "#include <array>");
    text_line(writer, "#include <cstddef>");
    text_line(writer, "#include <cstdint>");
    text_line(writer, "#include <string_view>");
    text_line(writer, "#include <refrain/net/base.hpp>");
    writer.blank();
    writer.line("namespace {} {{", name_space);
    writer.blank();
    text_line(writer, "/// One packet declared by a protocol schema. `id` is");
    text_line(writer, "/// unique within a (state, flow) pair.");
    text_line(writer, "struct packet_descriptor {");
    text_line(writer, "    ::refrain::net::connection_state state;");
    text_line(writer, "    ::refrain::net::packet_flow flow;");
    text_line(writer, "    std::uint16_t id;");
    text_line(writer, "    std::string_view name;");
    text_line(writer, "};");
    writer.blank();
    text_line(writer, "/// Locate a packet by wire id inside one stage table.");
    text_line(writer, "template <std::size_t N>");
    text_line(writer, "[[nodiscard]] constexpr auto find_packet("
                      "const std::array<packet_descriptor, N> &table,");
    text_line(writer, "                                      "
                      "::refrain::net::packet_flow flow,");
    text_line(
        writer,
        "                                      std::uint16_t id) noexcept");
    text_line(writer, "    -> const packet_descriptor * {");
    writer.indent();
    text_line(writer, "for (const auto &entry : table) {");
    writer.indent();
    text_line(writer, "if (entry.flow == flow && entry.id == id) {");
    writer.indent();
    text_line(writer, "return &entry;");
    writer.dedent();
    text_line(writer, "}");
    writer.dedent();
    text_line(writer, "}");
    text_line(writer, "return nullptr;");
    writer.dedent();
    text_line(writer, "}");
    writer.blank();
    text_line(writer,
              "/// Locate a packet by declared name inside one stage table.");
    text_line(writer, "template <std::size_t N>");
    text_line(writer, "[[nodiscard]] constexpr auto find_packet("
                      "const std::array<packet_descriptor, N> &table,");
    text_line(writer, "                                      "
                      "::refrain::net::packet_flow flow,");
    text_line(writer,
              "                                      std::string_view name) "
              "noexcept");
    text_line(writer, "    -> const packet_descriptor * {");
    writer.indent();
    text_line(writer, "for (const auto &entry : table) {");
    writer.indent();
    text_line(writer, "if (entry.flow == flow && entry.name == name) {");
    writer.indent();
    text_line(writer, "return &entry;");
    writer.dedent();
    text_line(writer, "}");
    writer.dedent();
    text_line(writer, "}");
    text_line(writer, "return nullptr;");
    writer.dedent();
    text_line(writer, "}");
    writer.blank();
    writer.line("}} // namespace {}", name_space);
    writer.blank();
    writer.line("#endif // {}", guard);
    return writer.take();
}

[[nodiscard]] auto emit_stage_header(const stage_catalog &stage,
                                     std::string_view schema_hash)
    -> std::string {
    const auto dir{"cpp/refrain/schema/protocol/" + stage.layout.version};
    const auto path{dir + "/" + stage.layout.stage + ".hpp"};
    const auto guard{emit::guard_for(path)};
    code_writer writer;
    writer.raw(emit::banner(schema_hash));
    writer.line("#ifndef {}", guard);
    writer.line("#define {}", guard);
    writer.blank();
    text_line(writer, "#include <array>");
    text_line(writer, "#include <cstdint>");
    text_line(writer, "#include <string_view>");
    text_line(writer, "#include <refrain/net/base.hpp>");
    writer.line("#include {}", generated_include(dir + "/packet.hpp"));
    for (const auto &header : stage.flow_headers) {
        writer.line("#include {}", header);
    }
    writer.blank();
    writer.line("namespace {} {{", stage.layout.name_space);
    writer.blank();
    auto packets{stage.packets};
    std::ranges::sort(packets, [](const auto &lhs, const auto &rhs) {
        if (lhs.layout.flow != rhs.layout.flow) {
            return lhs.layout.flow < rhs.layout.flow;
        }
        return lhs.id < rhs.id;
    });
    text_line(writer,
              "/// Packets declared for this stage, ordered by flow and id.");
    writer.line("inline constexpr std::array<packet_descriptor, {}> "
                "{}_packets{{{{",
                packets.size(), stage.layout.stage);
    writer.indent();
    for (const auto &packet : packets) {
        writer.line("packet_descriptor{{::refrain::net::connection_state::{}, "
                    "::refrain::net::packet_flow::{}, 0x{:X}, \"{}\"}},",
                    stage.layout.stage, packet.layout.flow,
                    static_cast<std::uint64_t>(packet.id),
                    emit::escape_text(packet.name));
    }
    writer.dedent();
    text_line(writer, "}};");
    writer.blank();
    writer.line("}} // namespace {}", stage.layout.name_space);
    writer.blank();
    writer.line("#endif // {}", guard);
    return writer.take();
}

[[nodiscard]] auto emit_catalog_header(const std::string &version,
                                       const std::string &name_space,
                                       const std::vector<stage_catalog> &stages,
                                       std::string_view schema_hash)
    -> std::string {
    const auto dir{"cpp/refrain/schema/protocol/" + version};
    const auto guard{emit::guard_for(dir + "/catalog.hpp")};
    code_writer writer;
    writer.raw(emit::banner(schema_hash));
    writer.line("#ifndef {}", guard);
    writer.line("#define {}", guard);
    writer.blank();
    text_line(writer, "#include <cstdint>");
    text_line(writer, "#include <string_view>");
    text_line(writer, "#include <refrain/net/base.hpp>");
    for (const auto &stage : stages) {
        writer.line("#include {}",
                    generated_include(dir + "/" + stage.layout.stage + ".hpp"));
    }
    writer.blank();
    writer.line("namespace {} {{", name_space);
    writer.blank();
    text_line(writer,
              "/// Packet table lookup across every stage of the protocol.");
    text_line(writer, "[[nodiscard]] constexpr auto find_packet("
                      "::refrain::net::connection_state state,");
    text_line(writer, "                                      "
                      "::refrain::net::packet_flow flow,");
    text_line(
        writer,
        "                                      std::uint16_t id) noexcept");
    text_line(writer, "    -> const packet_descriptor * {");
    writer.indent();
    text_line(writer, "switch (state) {");
    for (const auto &stage : stages) {
        writer.line("case ::refrain::net::connection_state::{}:",
                    stage.layout.stage);
        writer.indent();
        writer.line("return ::{}::find_packet({}_packets, flow, id);",
                    name_space, stage.layout.stage);
        writer.dedent();
    }
    text_line(writer, "}");
    text_line(writer, "return nullptr;");
    writer.dedent();
    text_line(writer, "}");
    writer.blank();
    text_line(writer, "[[nodiscard]] constexpr auto find_packet("
                      "::refrain::net::connection_state state,");
    text_line(writer, "                                      "
                      "::refrain::net::packet_flow flow,");
    text_line(writer,
              "                                      std::string_view name) "
              "noexcept");
    text_line(writer, "    -> const packet_descriptor * {");
    writer.indent();
    text_line(writer, "switch (state) {");
    for (const auto &stage : stages) {
        writer.line("case ::refrain::net::connection_state::{}:",
                    stage.layout.stage);
        writer.indent();
        writer.line("return ::{}::find_packet({}_packets, flow, name);",
                    name_space, stage.layout.stage);
        writer.dedent();
    }
    text_line(writer, "}");
    text_line(writer, "return nullptr;");
    writer.dedent();
    text_line(writer, "}");
    writer.blank();
    writer.line("}} // namespace {}", name_space);
    writer.blank();
    writer.line("#endif // {}", guard);
    return writer.take();
}

} // namespace

auto emit_rsc(const model &program, std::string_view schema_hash)
    -> std::vector<emit::generated_file> {
    std::vector<emit::generated_file> files;
    std::map<std::string, stage_catalog> stages;
    std::map<std::string, std::string> version_namespaces;
    for (std::uint32_t module_index = 0; module_index < program.modules.size();
         ++module_index) {
        const auto &module{program.modules[module_index]};
        if (module.kind != dialect::rsc) {
            continue;
        }
        std::vector<std::string> paths;
        for (const auto &decl : module.declarations) {
            if (decl.source_path.empty()) {
                continue;
            }
            if (std::ranges::find(paths, decl.source_path) == paths.end()) {
                paths.push_back(decl.source_path);
            }
        }
        std::sort(paths.begin(), paths.end());
        for (const auto &source_path : paths) {
            files.push_back(emit::generated_file{
                emit::rsc_cpp_header_path(source_path),
                emit_header(program, module_index, source_path, schema_hash)});
        }
        const auto layout{protocol_layout_of(module.name)};
        if (!layout.has_value()) {
            continue;
        }
        auto &stage{stages[layout->version + "/" + layout->stage]};
        stage.layout = *layout;
        version_namespaces[layout->version] = layout->name_space;
        for (const auto &source_path : paths) {
            stage.flow_headers.insert(emit::rsc_cpp_include(source_path));
        }
        for (const auto &decl : module.declarations) {
            if (decl.kind == declaration_kind::packet &&
                decl.packet_id.has_value()) {
                stage.packets.push_back(catalog_packet{
                    .layout = *layout,
                    .name = decl.name,
                    .id = static_cast<std::uint16_t>(*decl.packet_id)});
            }
        }
    }
    for (const auto &[version, name_space] : version_namespaces) {
        files.push_back(emit::generated_file{
            "cpp/refrain/schema/protocol/" + version + "/packet.hpp",
            emit_packet_header(version, name_space, schema_hash)});
        std::vector<stage_catalog> version_stages;
        for (const auto &[key, stage] : stages) {
            if (stage.layout.version == version) {
                version_stages.push_back(stage);
            }
        }
        std::ranges::sort(version_stages, [](const stage_catalog &lhs,
                                             const stage_catalog &rhs) {
            return lhs.layout.stage < rhs.layout.stage;
        });
        for (const auto &stage : version_stages) {
            files.push_back(
                emit::generated_file{"cpp/refrain/schema/protocol/" + version +
                                         "/" + stage.layout.stage + ".hpp",
                                     emit_stage_header(stage, schema_hash)});
        }
        if (!version_stages.empty()) {
            files.push_back(emit::generated_file{
                "cpp/refrain/schema/protocol/" + version + "/catalog.hpp",
                emit_catalog_header(version, name_space, version_stages,
                                    schema_hash)});
        }
    }
    std::sort(
        files.begin(), files.end(),
        [](const emit::generated_file &lhs, const emit::generated_file &rhs) {
            return lhs.path < rhs.path;
        });
    return files;
}

} // namespace refrain::schema
