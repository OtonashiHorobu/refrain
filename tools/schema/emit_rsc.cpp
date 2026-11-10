// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <schema/code_writer.hpp>
#include <schema/emit.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
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
        case primitive_kind::opaque_ptr:
        case primitive_kind::mut_bytes:
            // RIDL-only primitives; sema rejects them in RSC data schemas.
            std::unreachable();
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

[[nodiscard]] auto emit_header(const model &program, std::uint32_t module_index,
                               std::string_view source_path,
                               std::string_view schema_hash) -> std::string {
    const auto &module{program.modules[module_index]};
    const auto declarations{
        file_declarations(program, module_index, source_path)};
    const auto path{emit::rsc_cpp_header_path(source_path)};
    const auto guard{emit::guard_for(path)};

    std::set<std::string> includes;
    for (const auto *decl : declarations) {
        for (const auto &field : decl->fields) {
            collect_includes(program, field.type, includes);
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

    writer.line("}} // namespace {}", emit::cpp_namespace(module.name));
    writer.blank();
    writer.line("#endif // {}", guard);
    return writer.take();
}

} // namespace

auto emit_rsc(const model &program, std::string_view schema_hash)
    -> std::vector<emit::generated_file> {
    std::vector<emit::generated_file> files;
    for (std::uint32_t module_index = 0; module_index < program.modules.size();
         ++module_index) {
        if (program.modules[module_index].kind != dialect::rsc) {
            continue;
        }
        std::vector<std::string> paths;
        for (const auto &decl : program.modules[module_index].declarations) {
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
    }
    std::sort(
        files.begin(), files.end(),
        [](const emit::generated_file &lhs, const emit::generated_file &rhs) {
            return lhs.path < rhs.path;
        });
    return files;
}

} // namespace refrain::schema
